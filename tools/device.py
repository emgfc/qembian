#!/usr/bin/env python3
"""device.py: the device model (docs/devices.md, models/README.md). Loads a phone model
(models/<model>/model.toml, with its family and `extends`) with one of its firmware overlays
(models/<model>/firmware/<fw>/firmware.toml) merged on top, plus the [overrides] of a device instance
($SYBORG_INSTANCE, tools/instance.py), and answers the tools that need device facts, so no script
carries an N95 literal.

  tools/device.py list                         known devices (id, model, level)
  tools/device.py check [ID]                   schema, inheritance, files, keymap names/collisions
  tools/device.py show [ID] [--json]           the merged profile
  tools/device.py get [ID] KEY                 one value by dotted key (board.ram_mb)
  tools/device.py file [ID] NAME               a profile file (gl/gles-patches.txt): firmware overlay, else model
  tools/device.py paths [ID] [NAME]            build/firmware paths (dir, core_flat, rom, dtb, ...)
  tools/device.py qemu-args [ID] [--rom ROM]   board arguments for the one QEMU binary
  tools/device.py gen keymap-header [ID]       the guest EKEYB table (KConvertCode lines)
  tools/device.py gen key-aliases [ID]         sydb key name -> QEMU qcode, as JSON
  tools/device.py gen qemu-keymap [ID]         "qcode scancode" lines for syborg-keyboard.keymap
  tools/device.py gen standins-bldinf [ID]     bld.inf of the device's kernel-side stand-ins (src/standins)
  tools/device.py gen standins-oby [ID]        "kind name ext" per stand-in, in ROM order
  tools/device.py identify [DIR|ID]            which profile an extracted firmware (core-flat.img, rofs*.img)
                                               is: sw.txt/langsw.txt, image hashes, hardware drivers
  tools/device.py build [ID] [--variant V] [--kernel] [--no-standins] [--out IMG] [--force]
                                               compose the device ROM (replaces hybrid-boot.sh
                                               --compose-only / compose-exp.sh)

ID: a model (its default firmware), <model>@<firmware>, a firmware's own [device] id (nokia-n8-rm596-v13)
or an alias from [device].aliases. Default: $SYBORG_DEVICE, else the instance's device ($SYBORG_INSTANCE),
else models/registry.toml `default`. Build output: build/roms/<id>/<firmware key>/ (paths dir).
Stdlib only (tomllib), so the python3 -I tools can import it.
"""
import argparse, json, os, re, shutil, struct, sys, tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STDKEYS = os.path.join(ROOT, 'platform', 'symbian', 'stdkeys.toml')


class DeviceError(Exception):
    pass


# ---------------------------------------------------------------- loading and inheritance

MODELS = os.path.join(ROOT, 'models')
_INDEX = None


def _toml(path):
    with open(path, 'rb') as f:
        return tomllib.load(f)


def _index():
    """device id -> (model, firmware name or None, toml path, directory). A model's id loads it with its
    default firmware; a firmware overlay with [device] id (the profiles of before device model v2, e.g.
    nokia-n8-rm596-v13) is listed under that id; any other firmware is reached as <model>@<firmware>."""
    global _INDEX
    if _INDEX is None:
        idx = {}
        for m in sorted(os.listdir(MODELS)) if os.path.isdir(MODELS) else ():
            mt = os.path.join(MODELS, m, 'model.toml')
            if not os.path.isfile(mt):
                continue
            idx[m] = (m, None, mt, os.path.join(MODELS, m))
            fd = os.path.join(MODELS, m, 'firmware')
            for fw in sorted(os.listdir(fd)) if os.path.isdir(fd) else ():
                ft = os.path.join(fd, fw, 'firmware.toml')
                if os.path.isfile(ft):
                    vid = _toml(ft).get('device', {}).get('id')
                    if vid and vid != m:
                        idx[vid] = (m, fw, ft, os.path.join(fd, fw))
        _INDEX = idx
    return _INDEX


def device_ids():
    return sorted(_index())


def firmware_names(model):
    """the firmware overlays a model has (models/<model>/firmware/<name>/firmware.toml)"""
    fd = os.path.join(MODELS, model, 'firmware')
    return sorted(f for f in os.listdir(fd) if os.path.isfile(os.path.join(fd, f, 'firmware.toml'))) if os.path.isdir(fd) else []


REGISTRY = os.path.join(ROOT, 'models', 'registry.toml')


def registry():
    """the known type codes (models/registry.toml, docs/device-model-v2.md 2.1): code -> row"""
    with open(REGISTRY, 'rb') as f:
        return {r['code']: r for r in tomllib.load(f).get('types', [])}


def default_device():
    """models/registry.toml `default`: the device of a bare command"""
    with open(REGISTRY, 'rb') as f:
        return tomllib.load(f)['default']


# ---------------------------------------------------------------- device instances (tools/instance.py)
# An instance is one virtual phone (docs/device-model-v2.md 2.3): a directory with instance.toml (model,
# firmware, settings, [overrides]) and the session's state. $SYBORG_INSTANCE (its directory) makes every
# tool load the instance's device with its overrides; sydb sets it for --instance / an instance session.
# [overrides] may only touch tables that reach QEMU at start, never what is composed into the ROM.
RUNTIME_OVERRIDES = {'board': None, 'gl': ('host',), 'modem': None, 'sim': None, 'standins': ('name', 'config')}


def instance_file(idir=None):
    """the instance.toml of $SYBORG_INSTANCE (or idir), None when there is none"""
    idir = idir or os.environ.get('SYBORG_INSTANCE')
    p = idir and os.path.join(idir, 'instance.toml')
    return _toml(p) if p and os.path.isfile(p) else None


def instance_device(inst):
    """the device id an instance runs: its model with its firmware"""
    fw = inst.get('firmware') or ''
    return resolve('%s@%s' % (inst['model'], fw) if fw else inst['model'])


def override_problems(over):
    """what is wrong with an instance's [overrides] (tables not applied at run time)"""
    errs = []
    for k, v in over.items():
        if k not in RUNTIME_OVERRIDES:
            errs.append('[overrides] %s: not a run-time table (allowed: %s)' % (k, ', '.join(RUNTIME_OVERRIDES)))
            continue
        keys = RUNTIME_OVERRIDES[k]
        items = v if isinstance(v, list) else [v]
        for it in items:
            bad = [x for x in (it if isinstance(it, dict) else {}) if keys and x not in keys]
            if bad:
                errs.append('[overrides] %s: %s would change the composed ROM (allowed: %s)' % (k, ', '.join(bad), ', '.join(keys)))
    return errs


def apply_instance(prof, idir=None):
    """the profile with the instance's [overrides] merged on top (when the instance runs this device)"""
    idir = idir or os.environ.get('SYBORG_INSTANCE')
    inst = instance_file(idir)
    if not inst or instance_device(inst) != prof['_id']:
        return prof
    over = inst.get('overrides', {})
    errs = override_problems(over)
    if errs:
        raise DeviceError('instance %s: %s' % (inst.get('name'), '; '.join(errs)))
    if 'standins' in over:
        have = {s['name'] for s in prof.get('standins', [])}
        gone = [s.get('name') for s in over['standins'] if s.get('name') not in have]
        if gone:
            raise DeviceError('instance %s: [[overrides.standins]] %s: not a stand-in of %s' % (inst.get('name'), ', '.join(map(str, gone)), prof['_id']))
    prof = _merge(prof, over) if over else dict(prof)
    prof['_instance'] = os.path.abspath(idir)
    return prof


def resolve(dev=None):
    if not dev and not os.environ.get('SYBORG_DEVICE') and instance_file():
        return instance_device(instance_file())
    dev = dev or os.environ.get('SYBORG_DEVICE') or default_device()
    idx = _index()
    if dev == 'default':
        dev = default_device()
    if dev in idx:
        return dev
    if '@' in dev:					# <model>@<firmware>: any firmware the model has
        m, fw = dev.split('@', 1)
        if m in idx and fw in firmware_names(idx[m][0]):
            vid = next((i for i, v in idx.items() if v[0] == m and v[1] == fw), None)
            if vid:
                return vid
            if fw == _toml(idx[m][2]).get('firmware', {}).get('default'):
                return m
            idx[dev] = (m, fw, os.path.join(MODELS, m, 'firmware', fw, 'firmware.toml'), os.path.join(MODELS, m, 'firmware', fw))
            return dev
    for d in device_ids():
        prof = _read(d)
        if dev in prof.get('device', {}).get('aliases', []):
            return d
    raise DeviceError('unknown device %r (devices: %s)' % (dev, ', '.join(device_ids())))


def _read(dev):
    """the raw file of a device id (its firmware overlay, or the model for a bare model id)"""
    return _toml(_index()[dev][2])


def _merge(base, over):
    """tables deep-merge; scalars and plain arrays replace; arrays of tables merge by `name`
    (or `key`/`letter`/`role`), with an optional `remove = [names]` beside them; in a table, `remove`
    also drops the base's keys of that name (a variant whose firmware lacks a partition)"""
    out = dict(base)
    for k, v in over.items():
        if k == 'remove':
            continue
        b = out.get(k)
        if isinstance(v, dict) and isinstance(b, dict):
            out[k] = _merge(b, v)
        elif isinstance(v, list) and v and all(isinstance(x, dict) for x in v) and isinstance(b, list):
            idk = next((i for i in ('name', 'key', 'letter', 'role') if all(i in x for x in v)), None)
            if idk is None:
                out[k] = v
            else:
                merged = {x[idk]: x for x in b if isinstance(x, dict) and idk in x}
                for x in v:
                    merged[x[idk]] = _merge(merged.get(x[idk], {}), x)
                out[k] = list(merged.values())
        else:
            out[k] = v
    for gone in over.get('remove', []):
        if gone in out and not isinstance(out[gone], list):
            del out[gone]
        for k, v in list(out.items()):
            if isinstance(v, list):
                out[k] = [x for x in v if not (isinstance(x, dict) and gone in (x.get('name'), x.get('key')))]
    return out


def _load_model(m, seen):
    """a model with its family and the models it extends (no firmware)"""
    if m in seen:
        raise DeviceError('extends loop: %s' % ' -> '.join(seen + [m]))
    raw = _toml(os.path.join(MODELS, m, 'model.toml'))
    parent, fam = raw.get('extends'), raw.get('family')
    if parent:
        prof = _merge(_load_model(parent, seen + [m]), raw)
        prof.pop('extends', None)
        prof['_extends'] = parent
        prof['_dirs'] = [os.path.join(MODELS, m)] + prof.get('_dirs', [])
    else:
        prof = _merge(family(fam), raw) if fam else dict(raw)
        prof['_dirs'] = [os.path.join(MODELS, m)]
    return prof


def _load_firmware(m, fw, seen):
    """a firmware overlay of model m, over the firmwares it extends"""
    if fw in seen:
        raise DeviceError('firmware extends loop: %s' % ' -> '.join(seen + [fw]))
    d = os.path.join(MODELS, m, 'firmware', fw)
    raw = _toml(os.path.join(d, 'firmware.toml'))
    parent = raw.pop('extends', None)
    if parent:
        base, dirs = _load_firmware(m, parent, seen + [fw])
        return _merge(base, raw), [d] + dirs
    return raw, [d]


def load(dev=None, _seen=None):
    """a device's profile: its model (+ family, + the models it extends) with the firmware overlay merged
    on top (device model v2: models/<model>/model.toml + models/<model>/firmware/<fw>/firmware.toml)"""
    dev = resolve(dev)
    m, fw, _, _ = _index()[dev]
    prof = _load_model(m, [])
    fw = fw or get(prof, 'firmware.default')
    dirs = []
    if fw and os.path.isfile(os.path.join(MODELS, m, 'firmware', fw, 'firmware.toml')):
        over, dirs = _load_firmware(m, fw, [])
        prof = _merge(prof, over)
    prof['_dirs'] = dirs + prof['_dirs']
    prof['_id'] = dev
    prof['_model'] = m
    prof['_firmware'] = fw
    prof['_dir'] = prof['_dirs'][0]
    return apply_instance(prof)


FAMILIES = os.path.join(ROOT, 'platform', 'families')


def family(name):
    """platform/families/<name>.toml: values every member of a phone family shares (docs/devices.md
    "Families"); merged under the device profile"""
    p = os.path.join(FAMILIES, name + '.toml')
    if not os.path.isfile(p):
        raise DeviceError('unknown family %r (%s)' % (name, ', '.join(f[:-5] for f in sorted(os.listdir(FAMILIES)) if f.endswith('.toml'))))
    with open(p, 'rb') as f:
        return tomllib.load(f)


def get(prof, key, default=None):
    v = prof
    for part in key.split('.'):
        if not isinstance(v, dict) or part not in v:
            return default
        v = v[part]
    return v


def device_file(prof, name):
    """a file named in the profile: in the firmware overlay's directory, else the model's, else its parents'"""
    for d in prof['_dirs']:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return os.path.join(prof['_dirs'][-1 if len(prof['_dirs']) == 1 else 0], name)


STORE = os.environ.get('QEMBIAN_ROMS') or os.path.join(ROOT, 'roms')
# what inventory.py extracts from a firmware: shared by every profile built on the same firmware, kept in
# build/firmware/<firmware key>/ and linked into the profile's build directory (device-model-v2 step 3)
FIRMWARE_ITEMS = ('core-flat.img', 'core.json', 'core-files.txt', 'kernel-drivers.txt', 'abi', 'symindex.json', 'ghidra')
FIRMWARE_PATTERNS = (r'rofs\d+\.img', r'uda-[a-z]\.img', r'[a-z0-9]+-partitions\.txt')


_STORE = None


def store_entries():
    """the ROM store's firmwares (roms/<RM>/<version>[~label]/rom.toml, tools/fw/store.py): id -> manifest"""
    global _STORE
    if _STORE is None:
        _STORE = _store_entries()
    return _STORE


def _store_entries():
    out = {}
    if not os.path.isdir(STORE):
        return out
    for tc in sorted(os.listdir(STORE)):
        if not re.fullmatch(r'RM-\d+', tc):
            continue
        for v in sorted(os.listdir(os.path.join(STORE, tc))):
            m = os.path.join(STORE, tc, v, 'rom.toml')
            if os.path.exists(m):
                with open(m, 'rb') as f:
                    out['%s/%s' % (tc, v)] = tomllib.load(f)
    return out


def firmware_id(prof, entries=None):
    """the profile's firmware in the store: [firmware] store, else the one unlabelled entry whose type code and
    version equal [firmware.identity].sw (None: not in the store)"""
    if get(prof, 'firmware.store'):
        return get(prof, 'firmware.store')
    sw = get(prof, 'firmware.identity.sw', {})
    tc, ver = sw.get('type_code', ''), re.sub(r'^V\s*', '', sw.get('version', '')).strip()
    entries = store_entries() if entries is None else entries
    c = [i for i, m in entries.items() if m.get('type_code') == tc and m.get('sw_version') == ver and '~' not in i]
    return c[0] if len(c) == 1 else None


def firmware_key(prof, entries=None):
    """<store id>/<language>+<customer>: the firmware combination the profile's [[firmware.files]] globs pick"""
    import fnmatch
    entries = store_entries() if entries is None else entries
    fid = firmware_id(prof, entries)
    if not fid:
        return None
    ids = {}
    for f in entries[fid].get('files', []):
        for ff in get(prof, 'firmware.files', []):
            if ff.get('role') != 'uda' and fnmatch.fnmatch(f['name'], ff.get('glob', '')) and f.get('role') in ('rofs2', 'rofs3'):
                ids[f['role']] = f.get('id') or os.path.splitext(f['name'])[0]
    return '%s/%s' % (fid, '+'.join(ids[r] for r in ('rofs2', 'rofs3') if r in ids) or 'core')


def firmware_dirs(prof):
    """store: the firmware's files; cache: what inventory.py extracts (build/firmware/<key>/); rom: the composed
    build of this profile on it (build/roms/<id>/<key, flat>/), the device's build directory (paths dir)"""
    key = firmware_key(prof)
    if not key:
        return None
    return {'key': key, 'store': os.path.join(STORE, *key.split('/')[:2]),
            'cache': os.path.join(ROOT, 'build', 'firmware', *key.split('/')),
            'rom': os.path.join(ROOT, 'build', 'roms', prof['_id'], key.replace('/', '_'))}


def is_firmware_item(name):
    return name in FIRMWARE_ITEMS or any(re.fullmatch(p, name) for p in FIRMWARE_PATTERNS)


def link_firmware(prof):
    """the firmware cache's items (build/firmware/<key>/) linked into the device's build directory
    (build/roms/<id>/<key>/) under their names"""
    fd = firmware_dirs(prof)
    if not fd:
        return None
    os.makedirs(fd['rom'], exist_ok=True)
    for n in os.listdir(fd['cache']) if os.path.isdir(fd['cache']) else ():
        if not is_firmware_item(n):
            continue
        dst = os.path.join(fd['rom'], n)
        rel = os.path.relpath(os.path.join(fd['cache'], n), fd['rom'])
        if os.path.islink(dst) and os.readlink(dst) == rel:
            continue
        if os.path.lexists(dst):
            if os.path.isdir(dst) and not os.path.islink(dst):
                shutil.rmtree(dst)
            else:
                os.remove(dst)
        os.symlink(rel, dst)
    return fd


def firmware_add(model, store_id, rofs2=None, rofs3=None, uda=None, name=None):
    """write models/<model>/firmware/<name>/firmware.toml for a ROM store entry (device model v2): its files
    (one language and one customer pack: the given ids, else the first), identity from the store manifest"""
    entries = store_entries()
    if store_id not in entries:
        raise DeviceError('%s is not in the ROM store (tools/fw/store.py list)' % store_id)
    man = entries[store_id]
    tc, ver = store_id.split('/')
    name = name or ver
    d = os.path.join(MODELS, model, 'firmware', name)
    if os.path.exists(os.path.join(d, 'firmware.toml')):
        raise DeviceError('%s already has firmware %s' % (model, name))
    pick = {}
    for role, want in (('core', None), ('rofs2', rofs2), ('rofs3', rofs3), ('uda', uda)):
        fs = [f for f in man.get('files', []) if f['role'] == role]
        f = next((f for f in fs if want and (f.get('id') == want or f['name'] == want)), fs[0] if fs else None)
        if f:
            pick[role] = f
    if 'core' not in pick:
        raise DeviceError('%s has no core file' % store_id)
    lines = ['# Firmware %s of %s, written by tools/device.py firmware-add from the ROM store entry %s.' % (name, model, store_id),
             '# Merged over models/%s/model.toml. Not booted yet unless [status] says so.' % model, '',
             '[firmware]', 'version = "%s"' % man['sw_version']]
    if '~' in ver:
        lines.append('store = "%s"' % store_id)
    for role in ('core', 'rofs2', 'rofs3', 'uda'):
        if role in pick:
            lines += ['', '[[firmware.files]]', 'role = "%s"' % role, 'glob = "%s"' % pick[role]['name']]
            if role == 'uda':
                lines += ['drive = "C"', 'optional = true']
    lines += ['', '[firmware.identity]', 'sw = { version = "%s", date = "%s", type_code = "%s" }' % (man['sw_version'], man.get('sw_date', ''), tc),
              '', '[status]', 'level = "L0-untested"', 'updated = "%s"' % __import__('time').strftime('%Y-%m-%d'),
              'open = ["not booted on this firmware yet"]', '']
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'firmware.toml'), 'w').write('\n'.join(lines))
    global _INDEX
    _INDEX = None
    return '%s@%s' % (model, name)


def build_dir(prof):
    """the device's build directory: build/roms/<id>/<firmware key, flat>/ (the firmware's extracted items are
    linked in, the composed ROM, DTB and generated files are written there); build/roms/<id>/no-firmware/
    for a profile whose firmware is not in the ROM store"""
    fd = firmware_dirs(prof)
    return fd['rom'] if fd else os.path.join(ROOT, 'build', 'roms', prof['_id'], 'no-firmware')


def paths(prof):
    """build and firmware locations"""
    bdir = build_dir(prof)
    return {
        'dir': bdir,
        'core_flat': os.path.join(bdir, 'core-flat.img'),
        'core_json': os.path.join(bdir, 'core.json'),
        'rofs': [os.path.join(bdir, 'rofs%d.img' % i) for i in range(1, 2 + max(
            [int(f[4:-4]) for f in (os.listdir(bdir) if os.path.isdir(bdir) else ()) if re.fullmatch(r'rofs\d+\.img', f)] or [2]))
                 if i <= 2 or os.path.exists(os.path.join(bdir, 'rofs%d.img' % i))],
        'rom': os.path.join(bdir, 'hybrid.img'),
        'manifest': os.path.join(bdir, get(prof, 'rom.manifest', 'hybrid-manifest.txt')),
        'dtb': os.path.join(bdir, 'syborg-device.dtb'),
        'symindex': os.path.join(bdir, 'symindex.json'),
    }


def paths_v2(prof):
    """paths() plus where the firmware lives (store) and is extracted (cache), device-model-v2 step 3"""
    p = paths(prof)
    fd = firmware_dirs(prof) or {}
    p.update({'firmware_key': fd.get('key', ''), 'store': fd.get('store', ''), 'cache': fd.get('cache', ''),
              'rom_dir': fd.get('rom', '')})
    return p


# programs the firmware's EStart starts by bare name (loader: C: before Z:) that the sydb boot hook can
# stand in for, preferred first: HALSettings (9.2 and later), LanguageSelection (9.1). docs/agent-autostart.md
BOOT_HOOK_NAMES = ('HALSettings.exe', 'LanguageSelection.exe')


def boot_hook_name(prof):
    """the name the sydb boot hook takes on C: for this firmware, read from its EStart.exe (None: none fits)"""
    ps = paths(prof)
    with open(ps['core_json']) as f:
        cj = json.load(f)
    es = [x for x in cj['files'] if x['path'].lower() == 'sys/bin/estart.exe']
    if not es:
        return None
    with open(ps['core_flat'], 'rb') as f:
        f.seek(es[0]['addr'] - cj['base'])
        code = f.read(es[0]['size'])
    for n in BOOT_HOOK_NAMES:
        if n.encode('utf-16-le') in code:
            return n
    return None


# ---------------------------------------------------------------- keymap

def stdkeys():
    with open(STDKEYS, 'rb') as f:
        return tomllib.load(f)['keys']


def keymap(prof):
    p = device_file(prof, get(prof, 'board.input.keymap', 'keymap.toml'))
    with open(p, 'rb') as f:
        return tomllib.load(f)


def keymap_header(prof):
    """the guest EKEYB KConvertCode table: the Syborg base table (platform PC keys, patches 0005/0012)
    overlaid with the device keymap's set1 entries"""
    km = keymap(prof)
    base = os.path.join(ROOT, 'platform', 'symbian', 'syborg-pc-keys.toml')
    with open(base, 'rb') as f:
        table = {int(k, 16): v for k, v in tomllib.load(f)['slots'].items()}
    if get(prof, 'board.input.translation') == 'qemu':
        km = {}		# QEMU translates every mapped key: the guest table is the platform's, the same
        			# for every device, so devices sharing a baseport build no longer overwrite it
    for k in km.get('keys', []):
        if 'set1' in k:
            table[k['set1']] = k['scancode']
    for sw in km.get('switches', []):		# slide events (open/close send a key today)
        for pos in ('open', 'close'):
            e = sw.get(pos)
            if isinstance(e, dict) and 'set1' in e:
                table[e['set1']] = e['scancode']
    n = max(table) + 1 if table else 0
    lines = []
    for i in range(max(n, 0xf0)):
        lines.append('\t/*%02x*/ %s,' % (i, table.get(i, 'EStdKeyNull')))
    return lines


def key_aliases(prof):
    """sydb key name -> QEMU qcode: the keymap's keys, its [aliases], and slide switch layouts
    (landscape/portrait = the switch position that sets that layout)"""
    km = keymap(prof)
    out = {k['key']: k['host'][0] for k in km.get('keys', []) if k.get('host')}
    for name, target in km.get('aliases', {}).items():
        if target in out:
            out[name] = out[target]
    for sw in km.get('switches', []):
        for pos in ('open', 'close'):
            e = sw.get(pos)
            if isinstance(e, dict) and e.get('host'):
                out['%s-%s' % (sw['name'], pos)] = e['host'][0]
                if e.get('layout'):
                    out[e['layout']] = e['host'][0]
    return out


# ---------------------------------------------------------------- stand-ins (src/standins)

STANDINS = os.path.join(ROOT, 'src', 'standins')


def standin_mmp(name):
    for d in sorted(os.listdir(STANDINS)):
        p = os.path.join(STANDINS, d, name + '.mmp')
        if os.path.exists(p):
            return '%s/%s.mmp' % (d, name)
    raise DeviceError('stand-in %s: no src/standins/*/%s.mmp' % (name, name))


def standins_bldinf(prof):
    mmps = [standin_mmp(s['name']) for s in prof.get('standins', [])]
    lcd = get(prof, 'kernel.lcd.target')
    if lcd:
        mmps.append(standin_mmp(lcd))
    return ['// Generated by tools/device.py gen standins-bldinf for %s -- do not edit.' % prof['_id'],
            'PRJ_PLATFORMS', 'ARMV5', '', 'PRJ_MMPFILES'] + mmps


def standins_oby(prof):
    """one "obykind file ext" line per stand-in: ldd/pdd are device[], extensions extension[] .pdd; file = the
    ROM base name (`file`, default `name`: two stand-ins may share one, e.g. accelerometer.ldd + .pdd)"""
    out = []
    for s in prof.get('standins', []):
        k = s['kind']
        out.append({'ldd': 'device %s ldd', 'pdd': 'device %s pdd', 'extension': 'extension %s pdd',
                    'kext': 'extension %s dll', 'kext-ldd': 'extension %s ldd'}[k] % s.get('file', s['name']))
    return out


# ---------------------------------------------------------------- firmware identity

def _sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def _identity_mod():
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
    import identity		# tools/fw/identity.py: the version-file and image readers
    return identity


def _version_file(text):
    return _identity_mod().version_file(text)


def firmware_facts(bdir):
    """what an extracted firmware says about itself (build/roms/<id>/<fw>/ or any dir with the same files)"""
    idm = _identity_mod()
    facts = {'images': {}, 'sw': None, 'langsw': None, 'drivers': []}
    for name in ('core-flat.img', 'rofs1.img', 'rofs2.img'):
        p = os.path.join(bdir, name)
        if os.path.exists(p):
            facts['images'][name] = _sha256(p)
            if name.startswith('rofs'):
                for k, v in idm.rofs_versions(open(p, 'rb').read()).items():
                    if k in ('sw', 'langsw'):
                        facts[k] = v
    cj = os.path.join(bdir, 'core.json')
    if os.path.exists(cj):
        facts['drivers'] = sorted(f['path'].split('/')[-1].lower() for f in json.load(open(cj))['files']
                                  if f['path'].lower().endswith(('.ldd', '.pdd')))
    return facts


def identity_problems(prof, facts):
    """differences between the profile's [firmware.identity] and what the firmware says"""
    idn = get(prof, 'firmware.identity', {})
    out = []
    for k in ('sw', 'langsw'):
        want = idn.get(k, {})
        have = facts.get(k) or {}
        for f, v in want.items():
            norm = (lambda x: re.sub(r'^V\s*', '', x or '').strip()) if f == 'version' else (lambda x: x)
            if v and norm(have.get(f)) != norm(v):		# "" = not known yet; "V 35.0.001" = "35.0.001"
                out.append('%s.txt %s: profile %r, firmware %r' % (k, f, v, have.get(f)))
    for name, h in idn.get('sha256', {}).items():
        got = facts['images'].get(name)
        if h and got != h:
            out.append('%s: sha256 %s, profile %s' % (name, (got or 'missing')[:16], h[:16]))
    for drv in idn.get('drivers', []):
        if drv.lower() not in facts['drivers']:
            out.append('kernel driver %s not in the firmware (hardware the profile declares)' % drv)
    for drv in idn.get('no_drivers', []):
        if drv.lower() in facts['drivers']:
            out.append('kernel driver %s is in the firmware (the profile says absent)' % drv)
    return out


def identify(bdir):
    facts = firmware_facts(bdir)
    matches = []
    for d in device_ids():
        prof = load(d)
        if not get(prof, 'firmware.identity'):
            continue
        probs = identity_problems(prof, facts)
        matches.append((len(probs), d, probs))
    return facts, sorted(matches)


# ---------------------------------------------------------------- ROM composition

def _run(cmd, log=None):
    import subprocess
    if log:
        with open(log, 'w') as f:
            r = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    else:
        r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode:
        raise DeviceError('%s failed (%d)%s' % (' '.join(cmd[:2]), r.returncode, ', see ' + log if log else ''))


def build(prof, variant=None, kernel=False, standins=True, out=None, trace=None, force=False):
    """the ROM pipeline of scripts/hybrid-boot.sh, every argument from the profile"""
    ps = paths(prof)
    # the firmware must be the one the profile (its code patches, its addresses) was made for;
    # romcompose additionally checks every patch's original bytes
    probs = identity_problems(prof, firmware_facts(ps['dir']))
    if probs and not force:
        raise DeviceError('the firmware in %s is not the one %s describes:\n  %s\n(--force to compose anyway)'
                          % (ps['dir'], prof['_id'], '\n  '.join(probs)))
    logs = os.path.join(ROOT, 'build', 'logs')
    os.makedirs(logs, exist_ok=True)
    env_dev = {'SYBORG_DEVICE': prof['_id']}
    os.environ.update(env_dev)
    k = os.path.join('build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'kernel', 'eka')
    pers = get(prof, 'platform.personality')
    # exec numbering tables of every personality, compiled into the kernel, which picks one at boot
    # from the ROM header (personalities/registry.toml; docs/devices.md section 4.3)
    _run(['python3', '-I', 'tools/personality/gen_exec_map.py', k + '/kernel/execs.txt', k + '/kernel/execpers_map.h']
         + ['%s=personalities/%s/exec.map' % (p, p) for p in sorted(os.listdir(os.path.join(ROOT, 'personalities')))
            if os.path.isfile(os.path.join(ROOT, 'personalities', p, 'exec.map'))])
    bp = get(prof, 'kernel.baseport', 'syborg')	# syborg (multiple model) or a tools/baseport_variant.py one
    os.environ['SYBORG_BASEPORT'] = bp
    if kernel:
        _run(['scripts/build-syborg.sh', get(prof, 'kernel.variant', 'udeb')], os.path.join(logs, 'device-syborg-build.txt'))
    if standins:
        _run(['scripts/build-standins.sh', get(prof, 'kernel.variant', 'udeb'), prof['_id']],
             os.path.join(logs, 'device-standins-build.txt'))
    var = get(prof, 'rom.variants.%s' % variant) if variant else None
    if variant and var is None:
        raise DeviceError('%s has no ROM variant %r' % (prof['_id'], variant))
    img = out or os.path.join(ps['dir'], var.get('image', 'hybrid-%s.img' % variant) if var else 'hybrid.img')
    # code patches omitted by hack tag ("[H-...]" in the line): the default ROM's [rom] omit_patches, plus a variant's
    # own; a variant brings one back with restore_patches
    omit = set(get(prof, 'rom.omit_patches', []))
    if var:
        omit = (omit | set(var.get('omit_patches', []))) - set(var.get('restore_patches', []))
    omit = sorted(omit)
    pfiles = []
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
    import patches as patchmod		# tools/fw/patches.py: MODULE+OFFSET lines -> this firmware's addresses
    mods = patchmod.modules(ps['core_json'])
    for k, name in enumerate(get(prof, 'rom.code_patches', [])):
        src = device_file(prof, name)
        lines = open(src, newline='').read().split('\n')
        if omit:
            lines = [l for l in lines if not any('[%s]' % h in l for h in omit)]
        dst = os.path.join(ps['dir'], 'gen', 'code-patches-%d%s.txt' % (k, '-' + variant if variant else ''))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'w', newline='').write('\n'.join(patchmod.resolve_lines(lines, mods, src)))
        pfiles.append(dst)
    vr = get(prof, 'kernel.variant', 'udeb')
    cmd = ['python3', '-I', 'tools/romcompose.py',
           '--ours', os.path.join(ps['dir'], 'kernel-side.img'),
           '--n95', ps['core_flat'], '-o', img, '--manifest', ps['manifest'],
           '--trace', trace or hex(get(prof, 'kernel.trace_mask', 0x80000000))]
    cmd += ['--flash-mb', str(get(prof, 'board.flash.size_mb', 128))]
    if pers:
        cmd += ['--exec-personality', pers]		# personalities/registry.toml number in the ROM header
    for f in get(prof, 'rom.files', []):
        cmd += ['--file', '%s=%s' % (f['rom'], device_file(prof, f['local']))]
    for rp, gen in rom_file_edits(prof, ps):
        cmd += ['--file', '%s=%s' % (rp, gen)]
    rofs = [r for r in ps['rofs'] if os.path.exists(r)]
    rofs = rofs_patched(prof, ps, rofs)
    for r in rofs:
        cmd += ['--rofs', r]
    # a composite over several ROFS images, each on its own local drive (9.3: ROFS1/2/3 on 9/10/11)
    zdrives = [d['local_drive'] for d in get(prof, 'board.drives', []) if d.get('kind') == 'composite']
    if len(zdrives) > 1:
        if len(zdrives) != len(rofs):
            raise DeviceError('%d composite drives in [[board.drives]] for %d ROFS images' % (len(zdrives), len(rofs)))
        cmd += ['--rofs-drives', ','.join(str(x) for x in zdrives)]
    for pf in pfiles:
        cmd += ['--patches', pf]
    blob = os.path.join(ps['dir'], 'sygl', 'sygl.bin')			# src/sygl/build.sh --device ID
    if get(prof, 'gl.impl') == 'sygl' and os.path.exists(blob) and os.environ.get('SYGL', '1') == '1':
        addr = open(os.path.join(ps['dir'], 'sygl', 'sygl.addr')).read().strip()
        cmd += ['--patches', device_file(prof, get(prof, 'gl.entry_patches', 'gl/gles-patches.txt')),
                '--blob', '%s=%s' % (addr, blob)]
    gb = os.path.join(ps['dir'], 'gcebe', 'gcebe.bin')			# src/gcebe/build.sh --device ID
    if get(prof, 'composition.impl') == 'gcebe':
        if not os.path.exists(gb):
            raise DeviceError('%s: [composition] impl = "gcebe" but %s is missing: run src/gcebe/build.sh' % (prof['_id'], gb))
        addr = open(os.path.join(ps['dir'], 'gcebe', 'gcebe.addr')).read().strip()
        cmd += ['--patches', device_file(prof, get(prof, 'composition.patches')), '--blob', '%s=%s' % (addr, gb)]
    _run(cmd, os.path.join(logs, 'device-compose.txt'))
    disp = get(prof, 'board.display', {})
    _run(['python3', '-I', 'tools/dtb_set.py', 'external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb', ps['dtb'],
          'framebuffer:width=%d' % disp.get('width', 240), 'framebuffer:height=%d' % disp.get('height', 320)],
         os.path.join(logs, 'device-dtb.txt'))
    return img


def qemu_keymap(prof):
    """host qcode -> Symbian scan code, for QEMU's keyboard (board.input.translation = "qemu")"""
    keys = stdkeys()
    def code(sc):
        return ord(sc[1]) if re.fullmatch(r"'.'", sc) else keys[sc]
    km = keymap(prof)
    lines = ['# Generated by tools/device.py gen qemu-keymap for %s -- do not edit.' % prof['_id']]
    for k in km.get('keys', []):
        for h in k.get('host', []):
            lines.append('%s 0x%02x   # %s' % (h, code(k['scancode']), k['key']))
    for sw in km.get('switches', []):
        for pos in ('open', 'close'):
            e = sw.get(pos)
            if isinstance(e, dict):
                for h in e.get('host', []):
                    lines.append('%s 0x%02x   # %s %s' % (h, code(e['scancode']), sw['name'], pos))
    return lines


# ---------------------------------------------------------------- QEMU

GL_HOST_KEYS = {'msaa': (0, 2, 4, 8, 16), 'filter': ('auto', 'nearest', 'linear'), 'compose': ('host', 'guest'),
                'thread': ('on', 'off')}    # thread: GL on a worker thread (per boot; docs/gpu-boundary-roadmap.md phase 5)


def GL_HOST_DEFAULTS_OF(prof):
    """the profile's [gl.host] table (syborg-gl runtime settings), validated"""
    gl = get(prof, 'gl.host', {}) or {}
    for k, v in gl.items():
        if k not in GL_HOST_KEYS or v not in GL_HOST_KEYS[k]:
            raise DeviceError('[gl.host] %s = %r: one of %s' % (k, v, ', '.join('%s %s' % (n, list(o)) for n, o in GL_HOST_KEYS.items())))
    return gl


def qemu_args(prof, rom=None):
    b = prof.get('board', {})
    disp = b.get('display', {})
    a = ['-M', b.get('machine', 'syborg'), '-cpu', b.get('cpu', 'arm1136-r2'),
         '-m', '%dM' % b.get('ram_mb', 168),
         '-global', 'syborg-fb.width=%d' % disp.get('width', 240),
         '-global', 'syborg-fb.height=%d' % disp.get('height', 320)]
    a += ['-global', 'syborg-platform.device-id=%s' % prof['_id']]
    if get(prof, 'board.flash.size_mb', 128) != 128:	# the flash window (ROFS images; medrofs reads medrofs.flash_mb)
        a += ['-global', 'syborg-platform.flash-mb=%d' % get(prof, 'board.flash.size_mb')]
    if disp.get('touch'):				# touch phones: host mouse -> syborg-pointer (absolute) -> exyin stand-in
        a += ['-global', 'syborg-pointer.input=on']
        if disp.get('touch_rate_hz'):			# the panel's report rate (host moves merged between samples)
            a += ['-global', 'syborg-pointer.sample-hz=%d' % disp['touch_rate_hz']]
    sim = prof.get('sim', {})
    if sim.get('present') and sim.get('data', 'user'):	# packet data of the virtual SIM on a host network (syborg-modem)
        a += ['-nic', '%s,model=syborg-modem' % sim.get('data', 'user')]
    eth = get(prof, 'board.network.ethernet', None)
    if eth:						# syborg-net on a host network backend (user = slirp)
        a += ['-nic', '%s,model=syborg-net' % eth]
    cfg = board_config(prof)
    if cfg:
        a += ['-global', 'syborg-platform.config=%s' % cfg]
    # the fake modem's reply table (QEMU syborg-modem, read at start): written at every launch, so a reply
    # change needs no ROM rebuild ([modem].replies layering, tools/isi.py)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # run with -I: tools/ is not on the path
    import isi
    # an instance's [overrides] can change the modem: its table is its own
    tab = os.path.join(prof['_instance'], 'isi-replies.tab') if prof.get('_instance') else os.path.join(paths(prof)['dir'], 'gen', 'isi-replies.tab')
    os.makedirs(os.path.dirname(tab), exist_ok=True)
    text = isi.table_text(prof)
    if not os.path.exists(tab) or open(tab).read() != text:
        open(tab, 'w').write(text)
    a += ['-global', 'syborg-modem.replies=%s' % tab]
    # the GL identity (strings, extension lists) the host's OpenGL ES answers with ([gl.identity]): written at
    # every launch, so a change needs no ROM rebuild (docs/devices/nokia-n8-rm596/gles-host.md)
    ident = get(prof, 'gl.identity', {}) or {}
    if ident:
        idf = os.path.join(prof['_instance'], 'gl-identity.txt') if prof.get('_instance') else os.path.join(paths(prof)['dir'], 'gen', 'gl-identity.txt')
        os.makedirs(os.path.dirname(idf), exist_ok=True)
        text = ''.join('%s=%s\n' % (k, v) for k, v in sorted(ident.items()))
        if not os.path.exists(idf) or open(idf).read() != text:
            open(idf, 'w').write(text)
        a += ['-global', 'syborg-gl.identity=%s' % idf]
    # the EGL configs the host EGL serves ([gl] egl_configs: the real phone's, tools/egl-configs.py)
    if get(prof, 'gl.gpu_memory_mb'):	# the graphics memory EGL_NOK_resource_profiling reports (the memory monitor GOOM)
        a += ['-global', 'syborg-gl.gpu-memory-mb=%d' % get(prof, 'gl.gpu_memory_mb')]
    if get(prof, 'gl.egl_configs'):
        a += ['-global', 'syborg-gl.egl-configs=%s' % os.path.abspath(device_file(prof, get(prof, 'gl.egl_configs')))]
    # board tunables of ours, not phone facts: a profile sets them only when measured on the phone
    if get(prof, 'board.accelerometer.info'):	# the sensor's driver information block (syborg_sensor.c)
        a += ['-global', 'syborg-sensor.info=%s' % get(prof, 'board.accelerometer.info')]
    a += ['-global', 'syborg-fb.refresh-rate=%d' % disp.get('refresh_hz', 60)]
    a += ['-global', 'syborg-fb.blank-dim=%d' % disp.get('blank_dim_percent', 25)]
    # host GPU settings ([gl.host]; runtime-changeable with `sydb gl set`, docs/devices.md)
    for k, v in sorted(GL_HOST_DEFAULTS_OF(prof).items()):
        a += ['-global', 'syborg-gl.%s=%s' % (k, v)]
    if get(prof, 'board.input.translation') == 'qemu':
        km = os.path.join(paths(prof)['dir'], 'keymap.qemu')
        os.makedirs(os.path.dirname(km), exist_ok=True)
        text = '\n'.join(qemu_keymap(prof)) + '\n'
        if not os.path.exists(km) or open(km).read() != text:
            open(km, 'w').write(text)
        a += ['-global', 'syborg-keyboard.keymap=%s' % km]
    fl = b.get('flash')
    if rom and fl and os.path.exists(rom + '.rofs'):
        # read by the board into its flash window (syborg-platform.flash-file): the generic loader caps a
        # raw image at the RAM size, and Delight's ROFS (208 MB) is larger than the N8's 172 MB
        a += ['-global', 'syborg-platform.flash-file=%s.rofs' % rom]
    return a


def rom_file_edits(prof, ps):
    """[[rom.file_edits]]: a text file of the firmware's core ROM with one line inserted (path, after =
    the existing line it follows, insert, hack), written to build/roms/<id>/<fw>/gen/ from the firmware's
    own copy (no firmware text in the repository) and composed as a --file override."""
    edits = get(prof, 'rom.file_edits', [])
    if not edits:
        return []
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
    import probe
    out = []
    for e in edits:
        hits = [h for h in probe.find(prof['_id'], '^' + re.escape(e['path']) + '$') if h[0] == 'core']
        if not hits:
            raise DeviceError('rom.file_edits: no %s in the core ROM' % e['path'])
        body = hits[0][2]().decode('latin1')
        nl = '\r\n' if '\r\n' in body else '\n'
        lines = body.split(nl)
        at = [i for i, l in enumerate(lines) if l.strip() == e['after']]
        if len(at) != 1:
            raise DeviceError('rom.file_edits %s: line %r found %d times' % (e['path'], e['after'], len(at)))
        lines.insert(at[0] + 1, e['insert'])
        dst = os.path.join(ps['dir'], 'gen', os.path.basename(e['path']))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(nl.join(lines).encode('latin1'))
        print('%s: + %r after %r  [%s]' % (e['path'], e['insert'], e['after'], e.get('hack', '')))
        out.append((hits[0][1], dst))
    return out


def standin_binary(prof, name):
    """a stand-in as built for this device: epoc32/release/armv5/<variant>/_<baseport>_<name>"""
    p = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', get(prof, 'kernel.variant', 'udeb'),
                     '_%s_%s' % (get(prof, 'kernel.baseport', 'syborg'), name))
    if not os.path.exists(p):
        raise DeviceError('stand-in %s is not built (%s)' % (name, p))
    return p


def rofs_patched(prof, ps, rofs):
    """ROFS edits on a copy in build/roms/<id>/<fw>/gen/ (the firmware's own images stay as extracted):
    [[rom.rofs_patches]]: same-length edits of files inside a ROFS image. Each entry: image (1-based),
    path, old, new, encoding (default utf-16le, as CenRep .txt files), hack (id in hacks.md). The old
    text must occur exactly once in that file.
    [[rom.rofs_replace]]: a whole file replaced by one of ours (any size): image, path, built (path
    under build/roms/<id>/<fw>/), standin (one of our stand-in binaries by file name, as built for this
    device's kernel) or local (a profile file: firmware overlay, else model directory), hack. The new bytes are appended to
    the image; the directory entry gets their offset, size and UIDs, the header the new image size."""
    patches = get(prof, 'rom.rofs_patches', [])
    repl = get(prof, 'rom.rofs_replace', [])
    if not patches and not repl:
        return rofs
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import rofs as rofsmod
    out = list(rofs)
    for n in sorted({p['image'] for p in patches} | {r['image'] for r in repl}):
        data = bytearray(open(rofs[n - 1], 'rb').read())
        base, ents = rofsmod.files(bytes(data))
        if base and any(r['image'] == n for r in repl):	# same-length edits work there; appending does not
            raise DeviceError('ROFS image %d is a ROFx extension: rofs_replace is not supported there' % n)
        for p in (x for x in patches if x['image'] == n):
            enc = p.get('encoding', 'utf-16le')
            o, nw = p['old'].encode(enc), p['new'].encode(enc)
            if len(o) != len(nw):
                raise DeviceError('rofs patch %s: old and new differ in length' % p['path'])
            hit = [(off - base, size) for off, size, path in ents if path.lower() == p['path'].lower()]
            if not hit:
                raise DeviceError('rofs patch: no %s in ROFS image %d' % (p['path'], n))
            fo, fs = hit[0]
            if not 0 <= fo <= len(data) - fs:	# a ROFx entry whose data is in the image it extends
                raise DeviceError('rofs patch %s: its data is not in ROFS image %d' % (p['path'], n))
            body = bytes(data[fo:fo + fs])
            if body.count(o) != 1:
                raise DeviceError('rofs patch %s: %r found %d times (firmware changed?)' % (p['path'], p['old'], body.count(o)))
            i = fo + body.index(o)
            data[i:i + len(o)] = nw
            print('rofs%d %s: %r -> %r  [%s]' % (n, p['path'], p['old'], p['new'], p.get('hack', '')))
        for r in (x for x in repl if x['image'] == n):
            src = (os.path.join(ps['dir'], r['built']) if 'built' in r
                   else standin_binary(prof, r['standin']) if 'standin' in r else device_file(prof, r['local']))
            if not os.path.exists(src):
                raise DeviceError('rofs replace %s: %s is missing (build it first)' % (r['path'], src))
            new = open(src, 'rb').read()
            ent = rofsmod.entry(bytes(data), r['path'])
            if ent is None:
                raise DeviceError('rofs replace: no %s in ROFS image %d' % (r['path'], n))
            at = (len(data) + 3) & ~3
            data += b'\0' * (at - len(data)) + new
            if len(data) > struct.unpack_from('<I', data, 44)[0]:
                raise DeviceError('rofs replace %s: image %d grows past its iMaxImageSize' % (r['path'], n))
            struct.pack_into('<II', data, ent + 20, len(new), at)          # iFileSize, iFileAddress
            if new[16:20] == b'EPOC':                                      # E32 image: entry UIDs + check
                data[ent + 2:ent + 18] = new[0:16]
            struct.pack_into('<I', data, 36, len(data))                    # TRofsHeader::iImageSize
            print('rofs%d %s <- %s (%d bytes @0x%x)  [%s]' % (n, r['path'], os.path.relpath(src, ROOT), len(new), at, r.get('hack', '')))
        dst = os.path.join(ps['dir'], 'gen', 'rofs%d-patched.img' % n)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        out[n - 1] = dst
    return out


def board_config(prof):
    """stand-in settings the guest reads from the platform device (src/standins/_shared/boardconfig.h)"""
    m = prof.get('modem', {})
    kv = []
    if m.get('imei'):
        kv.append('modem.imei=%s' % m['imei'])
    if 'power_off_delay_s' in m:
        kv.append('modem.power_off_ms=%d' % int(m['power_off_delay_s'] * 1000))
    sim = prof.get('sim', {})					# the virtual SIM card in the fake modem (syborg_modem.c "virtual SIM")
    present = bool(sim.get('present'))
    if present:
        kv.append('sim.present=1')
        for key in ('imsi', 'mcc', 'mnc', 'spn'):
            if key in sim:
                kv.append('sim.%s=%s' % (key, sim[key]))
        for key in ('ip', 'dns'):				# the PDP address / DNS the network hands out (dotted IPv4)
            if key in sim:
                ip = str(sim[key])
                if len(ip.split('.')) != 4 or not all(x.isdigit() and int(x) < 256 for x in ip.split('.')):
                    raise DeviceError('[sim] %s = %r: not a dotted IPv4 address' % (key, ip))
                kv.append('sim.%s=%s' % (key, ip))
        for key in ('rat', 'lac', 'ci', 'attached'):
            if key in sim:
                kv.append('sim.%s=%d' % (key, int(sim[key])))
        if 'mtu' in sim:					# packet data MTU (576..1402: the phone's receive buffer)
            mtu = int(sim['mtu'])
            if not 576 <= mtu <= 1402:
                raise DeviceError('[sim] mtu = %d: must be 576..1402' % mtu)
            kv.append('sim.mtu=%d' % mtu)
    # unsolicited modem messages (iscpeer ProfileInds); `sim = true|false`: only with / without a SIM
    inds = [i for i in m.get('indications', []) if 'sim' not in i or bool(i['sim']) == present]
    if inds:
        kv.append('modem.ind=%s' % ','.join('%02x%02x%s%s%s' % (i['res'], i['msg'], i.get('data', '').replace(' ', '').lower(),
                                                                '@%d' % i['channel'] if 'channel' in i else '',
                                                                '>' + i['after'].replace(' ', '').lower() if 'after' in i else '')
                                             for i in inds))
    simst = m.get('sim_service_status', {})		# SIM_STATUS_RESP status per service (iscpeer; default 06)
    if m.get('sim_home_plmn'):				# answer SIM 2D {04} (home PLMN) in the S60 3rd TSY's layout
        kv.append('modem.hplmn=1')
    if m.get('sms_protocol'):				# SMS protocol variant of the phone's TSY (syborg_modem.c)
        kv.append('modem.sms=%s' % m['sms_protocol'])
    if simst:
        kv.append('modem.simst=%s' % ','.join('%02x:%02x' % (int(k, 0), v) for k, v in sorted(simst.items())))
    for st in prof.get('standins', []):			# the protocol variant each stand-in runs (stand-in reads
        for key, val in st.get('config', {}).items():	# "<name>.<key>" from the board config)
            kv.append('%s.%s=%s' % (st['name'], key, val))
    if get(prof, 'board.flash.size_mb', 128) != 128:
        kv.append('medrofs.flash_mb=%d' % get(prof, 'board.flash.size_mb'))
    audio = get(prof, 'board.audio', {})			# the DSP model: RAPIDO ring geometry (aamdaifdriver + the ISC peer's
    if 'keytone' in audio:				# the modem's key tone (syborg_modem.c modem_audio_params): HZ/MS/GAIN
        kv.append('audio.keytone=%s' % audio['keytone'])
    if audio.get('dsp') == 'rapido':			# virtual DSP clock), docs/devices/nokia-n8-rm596/rapido-audio.md
        kv.append('audio.ring_units=%d' % audio.get('ring_units', 16))
        # PCM format per stream type (= syborg-snd slot), declared per phone: RATE/CHANNELS/BITS/PLANAR (24 = 24 bits in
        # 32-bit words; PLANAR = chunks of N left then N right samples). Without a declaration the rings stay unbound
        # (silent, as before): a format measured on one phone is not assumed for another
        if 'streams' in audio:
            kv.append('audio.streams=%s' % audio['streams'])
        # F3 0x35 sent with this share of the ring still queued (the engine writes one 19200-byte block per
        # reply into a 30720-byte ring: 25 % keeps it fed without overwriting unplayed samples)
        kv.append('audio.lead_pct=%d' % audio.get('lead_pct', 25))
    disp = get(prof, 'board.display', {})			# d_display stand-in geometry (N95 values by default)
    for key, name in (('caps_words', 'd_display.caps'), ('native_orientation', 'd_display.native'),
                      ('line_pitch', 'd_display.pitch'), ('panel_rows', 'd_display.rows')):
        if key in disp:
            kv.append('%s=%s' % (name, disp[key]))
    return ';'.join(kv)


def drive_sizes(prof):
    return {d['kind']: d.get('size') for d in get(prof, 'board.drives', []) if d.get('image')}


# ---------------------------------------------------------------- user data layer (UDA)
# The flasher writes the user data area after the ROM layers: a FAT drive with the factory content
# (docs/devices.md "User data layer"). [[firmware.files]] role = "uda", drive = "C" declares it; the
# firmware inventory (tools/fw/inventory.py, --uda-only) extracts it to build/roms/<id>/<fw>/uda-<l>.img,
# a sparse image of the file system's size. A new session drive starts as a copy of it; SYBORG_UDA=0
# (sydb start/phone --no-uda) starts it blank, as before.

def uda_entries(prof):
    return [f for f in get(prof, 'firmware.files', []) if f.get('role') == 'uda']


def uda_image(prof, letter):
    """the extracted UDA image for a drive letter (path, may not exist yet) or None if none is declared"""
    for f in uda_entries(prof):
        if f.get('drive', 'C').upper() == letter.upper():
            return os.path.join(paths(prof)['dir'], 'uda-%s.img' % letter.lower())
    return None


def size_bytes(v):
    m = re.fullmatch(r'(\d+)([KMG]?)', str(v).strip().upper())
    if not m:
        raise DeviceError('bad size %r (want e.g. 64M)' % v)
    return int(m.group(1)) << {'': 0, 'K': 10, 'M': 20, 'G': 30}[m.group(2)]


def sparse_copy(src, dst):
    """copy keeping holes (a 255 MB UDA image holds ~100 MB of data, a blank one 1 MB)"""
    tmp = dst + '.tmp'
    with open(src, 'rb') as fi, open(tmp, 'wb') as fo:
        fo.truncate(os.path.getsize(src))
        zero = bytes(1 << 16)
        while True:
            at = fi.tell()
            b = fi.read(1 << 16)
            if not b:
                break
            if b != zero[:len(b)]:
                fo.seek(at)
                fo.write(b)
    os.replace(tmp, dst)


def drive_image(prof, kind, out, uda=None):
    """create a new drive image for the drive of this kind (nvmem = C:, nvmem2 = E:, ...): a copy of
    its UDA image when one is declared and the layer is on, else a blank file of the profile's size.
    Returns a one-line description."""
    if uda is None:
        uda = os.environ.get('SYBORG_UDA', '1') not in ('0', 'no', 'off', '')
    drv = [d for d in get(prof, 'board.drives', []) if d.get('kind') == kind and d.get('image')]
    if not drv:
        raise DeviceError('%s has no drive of kind %r' % (prof['_id'], kind))
    drv = drv[0]
    src = uda_image(prof, drv['letter'])
    if uda and src and os.path.exists(src):
        sparse_copy(src, out)
        return '%s: %s: user data layer %s (%d MB)' % (out, drv['letter'], os.path.relpath(src, ROOT),
                                                         os.path.getsize(out) >> 20)
    with open(out, 'wb') as f:
        f.truncate(size_bytes(drv.get('size', '64M')))
    why = ('off (SYBORG_UDA=0)' if not uda else 'not extracted yet (tools/fw/inventory.py --device %s --uda-only)'
           % prof['_id']) if src else 'none declared'
    return '%s: %s: blank %s (user data layer %s)' % (out, drv['letter'], drv.get('size', '64M'), why)


# ---------------------------------------------------------------- check

def check(prof):
    errs, warn = [], []
    for k in ('device', 'platform', 'board', 'kernel'):
        if k not in prof:
            errs.append('missing [%s]' % k)
    if get(prof, 'device.id') not in (prof['_id'], prof.get('_model') if '@' in prof['_id'] else None):
        errs.append('device.id %r does not match the directory %r' % (get(prof, 'device.id'), prof['_id']))
    try:
        km = keymap(prof)
        names = stdkeys()
        seen = {}
        for k in km.get('keys', []):
            sc = k.get('scancode', '')
            if sc not in names and not re.fullmatch(r"'.'", sc):
                errs.append('keymap: %s: unknown scan code %r' % (k.get('key'), k.get('scancode')))
            s = k.get('set1')
            if s is not None:
                if s in seen:
                    errs.append('keymap: set1 0x%x used by %s and %s' % (s, seen[s], k.get('key')))
                seen[s] = k.get('key')
                if s >= 0xf0:
                    errs.append('keymap: %s: set1 0x%x is past the 0xf0-entry guest table' % (k.get('key'), s))
    except FileNotFoundError as e:
        errs.append('keymap: %s' % e)
    for s in prof.get('standins', []):
        try:
            standin_mmp(s['name'])
        except DeviceError as e:
            errs.append(str(e))
    for p in get(prof, 'rom.code_patches', []):
        if not os.path.exists(device_file(prof, p)):
            errs.append('rom.code_patches: %s missing' % p)
    for f in get(prof, 'rom.files', []):
        if not os.path.exists(device_file(prof, f['local'])):
            errs.append('rom.files: %s missing' % f['local'])
    letters = {d['letter'].upper() for d in get(prof, 'board.drives', []) if d.get('image')}
    for f in uda_entries(prof):
        if f.get('drive', 'C').upper() not in letters:
            errs.append('firmware.files uda: drive %s has no image in [[board.drives]]' % f.get('drive', 'C'))
    pers = get(prof, 'platform.personality', '')
    reg = tomllib.load(open(os.path.join(ROOT, 'personalities', 'registry.toml'), 'rb'))['ids']
    if pers not in reg:
        errs.append('personality %r is not in personalities/registry.toml' % pers)
    elif pers != 'native' and not os.path.isdir(os.path.join(ROOT, 'personalities', pers)):
        errs.append('personality %r has a number but no personalities/%s/ (exec map, patches, kabi.toml)' % (pers, pers))
    return errs, warn


# ---------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    p = sub.add_parser('registry', help='known type codes (models/registry.toml)'); p.add_argument('code', nargs='?')
    p = sub.add_parser('firmware-add', help='a firmware overlay for a ROM store entry: models/<model>/firmware/<name>/')
    p.add_argument('model'); p.add_argument('store_id'); p.add_argument('--rofs2'); p.add_argument('--rofs3')
    p.add_argument('--uda'); p.add_argument('--name')
    sub.add_parser('firmwares', help='every model and the firmwares it has')
    for c in ('check', 'show', 'paths', 'qemu-args'):
        p = sub.add_parser(c)
        p.add_argument('dev', nargs='?')
        if c == 'show':
            p.add_argument('--json', action='store_true')
        if c == 'paths':
            p.add_argument('name', nargs='?')
        if c == 'qemu-args':
            p.add_argument('--rom')
            p.add_argument('--shell', action='store_true', help='one shell-quoted line')
    p = sub.add_parser('get'); p.add_argument('args', nargs='+')
    p = sub.add_parser('file', help="a profile file (gl/gles-patches.txt): the firmware overlay's, else the model's"); p.add_argument('args', nargs='+')
    p = sub.add_parser('build'); p.add_argument('dev', nargs='?'); p.add_argument('--variant')
    p.add_argument('--kernel', action='store_true', help='also rebuild the Syborg baseport/kernel side')
    p.add_argument('--no-standins', action='store_true'); p.add_argument('--out'); p.add_argument('--trace')
    p.add_argument('--force', action='store_true', help='compose even if the firmware identity does not match')
    p = sub.add_parser('drive-image', help='create a new drive image (UDA copy or blank)')
    p.add_argument('dev', nargs='?'); p.add_argument('--kind', required=True, help='nvmem (C:), nvmem2 (E:)')
    p.add_argument('--out', required=True); p.add_argument('--no-uda', action='store_true')
    p = sub.add_parser('identify'); p.add_argument('where', nargs='?', help='firmware dir (default: the device build dir)')
    p = sub.add_parser('gen'); p.add_argument('what', choices=['keymap-header', 'key-aliases', 'qemu-keymap', 'standins-bldinf', 'standins-oby'])
    p.add_argument('dev', nargs='?')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'list':
            for d in device_ids():
                pr = load(d)
                print('%-28s %-24s %s' % (d, '%s %s' % (get(pr, 'device.vendor', ''), get(pr, 'device.model', '')),
                                         get(pr, 'status.level', '')))
            return 0
        if a.cmd == 'firmware-add':
            print(firmware_add(a.model, a.store_id, a.rofs2, a.rofs3, a.uda, a.name))
            return 0
        if a.cmd == 'firmwares':
            for m in sorted({v[0] for v in _index().values()}):
                dflt = get(_load_model(m, []), 'firmware.default')
                print('%-24s %s' % (m, '  '.join(('*' if f == dflt else '') + f for f in firmware_names(m))))
            return 0
        if a.cmd == 'registry':
            reg = registry()
            rows = [reg[a.code.upper()]] if a.code else list(reg.values())
            for r in rows:
                print('%-7s %-20s %-10s %-22s %-14s %s' % (r['code'], r['name'], r.get('platform', ''), r.get('soc', ''),
                                                       r.get('status', ''), r.get('model', '')))
            return 0
        if a.cmd == 'identify':
            where = a.where
            if not where or not os.path.isdir(where):
                where = paths(load(where))['dir']
            facts, matches = identify(where)
            sw, lsw = facts.get('sw') or {}, facts.get('langsw') or {}
            print('firmware in %s' % where)
            print('  sw.txt     %s %s %s %s' % (sw.get('version'), sw.get('date'), sw.get('type_code'), sw.get('product')))
            print('  langsw.txt %s %s %s' % (lsw.get('version'), lsw.get('date'), lsw.get('type_code')))
            wl = [x for x in facts['drivers'] if x.startswith('wlan')]
            print('  WLAN drivers: %s' % (', '.join(wl) or 'none'))
            for n, d, probs in matches:
                print('%s %s%s' % ('MATCH' if n == 0 else '  -  ', d, '' if n == 0 else ': ' + '; '.join(probs[:3])))
            return 0 if matches and matches[0][0] == 0 else 1
        if a.cmd == 'get':
            dev, key = (a.args[0], a.args[1]) if len(a.args) > 1 else (None, a.args[0])
            v = get(load(dev), key)
            if v is None:
                print('no %s' % key, file=sys.stderr)
                return 1
            print(json.dumps(v) if isinstance(v, (dict, list)) else v)
            return 0
        if a.cmd == 'file':
            dev, name = (a.args[0], a.args[1]) if len(a.args) > 1 else (None, a.args[0])
            print(device_file(load(dev), name))
            return 0
        if a.cmd == 'paths' and a.dev and not a.name and a.dev in ('dir', 'core_flat', 'core_json', 'rofs', 'rom', 'manifest',
                                                                  'firmware_key', 'store', 'cache', 'rom_dir',
                                                                 'dtb', 'symindex'):
            a.dev, a.name = None, a.dev			# "paths dtb" = the default device's dtb
        prof = load(getattr(a, 'dev', None))
        if a.cmd == 'check':
            errs, warn = check(prof)
            for w in warn:
                print('warning: ' + w)
            for e in errs:
                print('error: ' + e)
            print('%s: %s' % (prof['_id'], 'ok' if not errs else '%d error(s)' % len(errs)))
            return 1 if errs else 0
        if a.cmd == 'show':
            if a.json:
                print(json.dumps(prof, indent=1, default=str))
            else:
                for k, v in prof.items():
                    print('%s = %s' % (k, json.dumps(v, default=str)[:200]))
            return 0
        if a.cmd == 'paths':
            ps = paths_v2(prof)
            if a.name:
                v = ps[a.name]
                print(v if isinstance(v, str) else ' '.join(v))
            else:
                for k, v in ps.items():
                    print('%-10s %s' % (k, v if isinstance(v, str) else ' '.join(v)))
            return 0
        if a.cmd == 'qemu-args':
            args = qemu_args(prof, a.rom)
            if a.shell:
                import shlex
                print(' '.join(shlex.quote(x) for x in args))
            else:
                print('\n'.join(args))
            return 0
        if a.cmd == 'gen' and a.what == 'keymap-header':
            print('\n'.join(keymap_header(prof)))
            return 0
        if a.cmd == 'drive-image':
            print(drive_image(prof, a.kind, a.out, False if a.no_uda else None))
            return 0
        if a.cmd == 'build':
            img = build(prof, a.variant, a.kernel, not a.no_standins, a.out, a.trace, a.force)
            print(open(os.path.join(ROOT, 'build', 'logs', 'device-compose.txt')).read().strip().splitlines()[-1])
            print('ROM: %s' % img)
            return 0
        if a.cmd == 'gen' and a.what == 'qemu-keymap':
            print('\n'.join(qemu_keymap(prof)))
            return 0
        if a.cmd == 'gen' and a.what == 'standins-bldinf':
            print('\n'.join(standins_bldinf(prof)))
            return 0
        if a.cmd == 'gen' and a.what == 'standins-oby':
            print('\n'.join(standins_oby(prof)))
            return 0
        if a.cmd == 'gen' and a.what == 'key-aliases':
            print(json.dumps(key_aliases(prof), indent=1))
            return 0
    except DeviceError as e:
        print('device.py: %s' % e, file=sys.stderr)
        return 2
    return 1


if __name__ == '__main__':
    sys.exit(main())
