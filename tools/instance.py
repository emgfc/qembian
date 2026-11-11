#!/usr/bin/env python3
"""instance.py: device instances, the user's virtual phones (docs/device-model-v2.md 2.3, docs/sydb.md).

An instance is one phone the way a VM is one machine: a name, a model, one of the model's firmwares,
its own drives, snapshots, logs and settings, and optionally a few [overrides] of the model profile
(run-time values only: board, gl.host, modem, stand-in config). It is a directory
build/instances/<name>/ ($QEMBIAN_HOME/instances when set) holding instance.toml and everything a sydb
session holds; `sydb --instance NAME ...` and `sydb phone NAME` run it. Instances of the same model and
firmware share one composed ROM (build/roms/<id>/<firmware key>/).

  tools/instance.py list [--all]                    instances (--all: also throw-away ones)
  tools/instance.py show NAME                       instance.toml, the device it resolves to, its ROM
  tools/instance.py get NAME KEY                    one value: dir, device, rom, drives, model, firmware, settings.audio ...
  tools/instance.py create NAME DEVICE [--primary] [--ephemeral] [--replace] [--variant V] [--audio A] [--gl G]
                                                    DEVICE: a model, MODEL@FIRMWARE or a firmware's id
                                                    (nokia-n8-rm596-delight); drives are made at the first start
  tools/instance.py clone SRC NEW                   config + drives (sparse copies); snapshots, logs not copied
  tools/instance.py rename OLD NEW
  tools/instance.py delete NAME --yes               the directory with its drives and snapshots
  tools/instance.py set NAME KEY VALUE              settings.gl|audio|scale, rom_variant, primary;
                                                    firmware FW --keep-drives|--wipe-drives
  tools/instance.py adopt DIR NAME [--primary]      make an existing sydb session directory an instance
                                                    (moved to the instances directory; its device mark says
                                                    which device it is)
  tools/instance.py check [NAME]                    instance.toml, device, overrides
  tools/instance.py primary [DEVICE]                the name of the device's primary instance (default device:
                                                    $SYBORG_DEVICE, else models/registry.toml `default`)

primary: the instance `SYBORG_DEVICE=<device> sydb ...` uses when no --instance / --session names one
(one per device). ephemeral: a throw-away instance (smoke tests), hidden from `list`.
Stdlib only, like tools/device.py.
"""
import argparse, datetime, os, shutil, subprocess, sys, tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import device as devmodel   # tools/device.py

SETTINGS = {'gl': ('host', 'soft'), 'audio': ('host', 'wav'), 'scale': float}
DEFAULT_SETTINGS = {'gl': 'host', 'audio': 'host', 'scale': 1.0}


class InstanceError(Exception):
    pass


def home():
    """the instances directory"""
    h = os.environ.get('QEMBIAN_HOME')
    return os.path.join(h, 'instances') if h else os.path.join(ROOT, 'build', 'instances')


def idir(name):
    return os.path.join(home(), name)


def valid_name(name):
    import re
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', name or ''):
        raise InstanceError('instance name %r: letters, digits, . _ - (not starting with . _ -)' % name)
    return name


def read(name_or_dir):
    d = name_or_dir if os.sep in name_or_dir else idir(name_or_dir)
    p = os.path.join(d, 'instance.toml')
    if not os.path.isfile(p):
        raise InstanceError('no instance %r (%s; tools/instance.py list)' % (name_or_dir, p))
    with open(p, 'rb') as f:
        inst = tomllib.load(f)
    inst['_dir'] = os.path.abspath(d)
    return inst


def instances(include_ephemeral=False):
    out = {}
    h = home()
    for n in sorted(os.listdir(h)) if os.path.isdir(h) else ():
        if os.path.isfile(os.path.join(h, n, 'instance.toml')):
            try:
                inst = read(os.path.join(h, n))
            except (tomllib.TOMLDecodeError, OSError):
                continue
            if include_ephemeral or not inst.get('ephemeral'):
                out[n] = inst
    return out


def device_id(inst):
    return devmodel.instance_device(inst)


def primary_for(dev):
    """the primary instance of a device id (None: there is none)"""
    for n, inst in instances(True).items():
        try:
            if inst.get('primary') and device_id(inst) == dev:
                return inst
        except devmodel.DeviceError:
            continue
    return None


def profile(inst):
    """the instance's merged profile: model + firmware + [overrides]"""
    return devmodel.apply_instance(devmodel.load(device_id(inst)), inst['_dir'])


def rom(inst, prof=None):
    """the composed ROM the instance boots: the variant's image, else hybrid.img"""
    prof = prof or profile(inst)
    v = inst.get('rom_variant') or ''
    d = devmodel.paths(prof)['dir']
    if not v:
        return os.path.join(d, 'hybrid.img')
    var = devmodel.get(prof, 'rom.variants', {}).get(v)
    if var is None:
        raise InstanceError('instance %s: %s has no ROM variant %r' % (inst['name'], prof['_id'], v))
    return os.path.join(d, var.get('image', 'hybrid-%s.img' % v))


def running(inst):
    try:
        pid = int(open(os.path.join(inst['_dir'], 'daemon.pid')).read())
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


# ---------------------------------------------------------------- writing instance.toml

def _q(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, list):
        return '[%s]' % ', '.join(_q(x) for x in v)
    if isinstance(v, dict):
        return '{ %s }' % ', '.join('%s = %s' % (k, _q(x)) for k, x in v.items())
    return '"%s"' % str(v).replace('\\', '\\\\').replace('"', '\\"')


def _table(name, t, out):
    plain = {k: v for k, v in t.items() if not isinstance(v, dict) and not (isinstance(v, list) and v and all(isinstance(x, dict) for x in v))}
    if plain or not t:
        out.append('[%s]' % name)
        out += ['%s = %s' % (k, _q(v)) for k, v in plain.items()]
        out.append('')
    for k, v in t.items():
        if isinstance(v, dict):
            _table('%s.%s' % (name, k), v, out)
        elif k not in plain:
            for x in v:
                out.append('[[%s.%s]]' % (name, k))
                out += ['%s = %s' % (kk, _q(vv)) for kk, vv in x.items()]
                out.append('')


def write(inst):
    d = inst['_dir']
    top = ['schema', 'name', 'created', 'model', 'firmware', 'rom_variant', 'primary', 'ephemeral', 'notes']
    out = ['# Device instance (tools/instance.py, docs/device-model-v2.md 2.3): one virtual phone. Its drives,',
           '# snapshots and logs are in this directory. Written by tools/instance.py; [overrides] may be edited',
           '# by hand (run-time tables only: board, gl.host, modem, [[overrides.standins]] name + config).', '']
    out += ['%s = %s' % (k, _q(inst[k])) for k in top if k in inst]
    out.append('')
    _table('settings', inst.get('settings', {}), out)
    if inst.get('overrides'):
        _table('overrides', inst['overrides'], out)
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, 'instance.toml.tmp')
    open(tmp, 'w').write('\n'.join(out).rstrip('\n') + '\n')
    os.replace(tmp, os.path.join(d, 'instance.toml'))


def new(name, dev, primary=None, ephemeral=False, variant='', settings=None, notes=None):
    """instance.toml for a device id (model, model@firmware or a firmware's own id)"""
    dev = devmodel.resolve(dev)
    prof = devmodel.load(dev)
    inst = {'schema': 1, 'name': valid_name(name), 'created': datetime.date.today().isoformat(),
            'model': prof['_model'], 'firmware': prof['_firmware'] or '', 'rom_variant': variant or '',
            'primary': bool(primary if primary is not None else (not ephemeral and primary_for(dev) is None)),
            'ephemeral': bool(ephemeral), 'settings': dict(DEFAULT_SETTINGS, **(settings or {})),
            '_dir': idir(name)}
    if notes:
        inst['notes'] = notes
    if inst['primary']:
        _unprimary(dev, name)
    rom(inst, prof)                     # the variant exists
    return inst


def _unprimary(dev, keep):
    for n, other in instances(True).items():
        if n != keep and other.get('primary'):
            try:
                same = device_id(other) == dev
            except devmodel.DeviceError:
                same = False
            if same:
                other['primary'] = False
                write(other)


def mark(inst):
    """the session's device mark (sydb refuses another phone's drives): the instance's device"""
    open(os.path.join(inst['_dir'], 'device'), 'w').write(device_id(inst) + '\n')


def sparse_copytree(src, dst, skip=('snaps', 'catches', 'shots', 'tmp')):
    os.makedirs(dst)
    for n in os.listdir(src):
        if n in skip or n.endswith('.sock') or n in ('daemon.pid', 'gdb.port', 'ram', 'frame.seq'):
            continue
        s, t = os.path.join(src, n), os.path.join(dst, n)
        if os.path.islink(s):
            raise InstanceError('%s is a link (%s): copy what it points to by hand' % (s, os.readlink(s)))
        if os.path.isdir(s):
            sparse_copytree(s, t, ())
        else:
            subprocess.run(['cp', '--sparse=always', '--reflink=auto', '-p', s, t], check=True)


def check_one(inst):
    errs = []
    for k in ('schema', 'name', 'model'):
        if k not in inst:
            errs.append('missing %s' % k)
    if os.path.basename(inst['_dir']) != inst.get('name'):
        errs.append('name %r is not the directory name %r' % (inst.get('name'), os.path.basename(inst['_dir'])))
    try:
        dev = device_id(inst)
        prof = profile(inst)
        rom(inst, prof)
        m = os.path.join(inst['_dir'], 'device')
        if os.path.exists(m) and open(m).read().strip() != dev:
            errs.append('drives are marked %s, the instance runs %s (tools/instance.py set NAME firmware FW '
                        '--keep-drives|--wipe-drives)' % (open(m).read().strip(), dev))
    except (devmodel.DeviceError, InstanceError) as e:
        errs.append(str(e))
    for k, v in inst.get('settings', {}).items():
        allowed = SETTINGS.get(k)
        if allowed is None:
            errs.append('settings.%s: unknown (known: %s)' % (k, ', '.join(SETTINGS)))
        elif isinstance(allowed, tuple) and v not in allowed:
            errs.append('settings.%s = %r: one of %s' % (k, v, ', '.join(allowed)))
    return errs


# ---------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(prog='instance.py', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list'); p.add_argument('--all', action='store_true')
    p = sub.add_parser('show'); p.add_argument('name')
    p = sub.add_parser('get'); p.add_argument('name'); p.add_argument('key')
    p = sub.add_parser('create'); p.add_argument('name'); p.add_argument('device')
    p.add_argument('--primary', action='store_true', default=None); p.add_argument('--ephemeral', action='store_true')
    p.add_argument('--replace', action='store_true', help='replace an existing throw-away (ephemeral) instance of that name')
    p.add_argument('--variant', default=''); p.add_argument('--audio', choices=SETTINGS['audio']); p.add_argument('--gl', choices=SETTINGS['gl'])
    p.add_argument('--notes')
    p = sub.add_parser('clone'); p.add_argument('src'); p.add_argument('new')
    p = sub.add_parser('rename'); p.add_argument('old'); p.add_argument('new')
    p = sub.add_parser('delete'); p.add_argument('name'); p.add_argument('--yes', action='store_true')
    p = sub.add_parser('set'); p.add_argument('name'); p.add_argument('key'); p.add_argument('value')
    p.add_argument('--keep-drives', action='store_true'); p.add_argument('--wipe-drives', action='store_true')
    p = sub.add_parser('adopt'); p.add_argument('dir'); p.add_argument('name'); p.add_argument('--primary', action='store_true', default=None)
    p.add_argument('--device', help='the device when the session has no device mark')
    p = sub.add_parser('check'); p.add_argument('name', nargs='?')
    p = sub.add_parser('primary'); p.add_argument('device', nargs='?')
    a = ap.parse_args(argv)
    try:
        return run(a)
    except (InstanceError, devmodel.DeviceError) as e:
        print('instance.py: %s' % e, file=sys.stderr)
        return 2


def run(a):
    if a.cmd == 'list':
        rows = instances(a.all)
        if not rows:
            print('no instances in %s (tools/instance.py create NAME DEVICE)' % home())
        for n, inst in rows.items():
            try:
                dev = device_id(inst)
            except devmodel.DeviceError as e:
                dev = 'ERROR: %s' % e
            flags = ','.join(f for f, on in (('primary', inst.get('primary')), ('ephemeral', inst.get('ephemeral')),
                                              ('running', running(inst))) if on)
            print('%-16s %-40s %-14s %s' % (n, dev, inst.get('rom_variant') or '-', flags))
        return 0
    if a.cmd == 'show':
        inst = read(a.name)
        print(open(os.path.join(inst['_dir'], 'instance.toml')).read().rstrip())
        print('# device %s, ROM %s%s' % (device_id(inst), os.path.relpath(rom(inst), ROOT), ', running' if running(inst) else ''))
        return 0
    if a.cmd == 'get':
        inst = read(a.name)
        v = {'dir': lambda: inst['_dir'], 'device': lambda: device_id(inst), 'rom': lambda: rom(inst),
             'drives': lambda: os.path.join(inst['_dir'], 'drives')}.get(a.key)
        if v:
            print(v())
            return 0
        cur = inst
        for k in a.key.split('.'):
            if k == 'settings' and k not in cur:
                cur = DEFAULT_SETTINGS
                continue
            if not isinstance(cur, dict) or k not in cur:
                if a.key.startswith('settings.') and k in DEFAULT_SETTINGS:
                    cur = DEFAULT_SETTINGS[k]
                    break
                raise InstanceError('%s has no %s' % (a.name, a.key))
            cur = cur[k]
        print(cur if not isinstance(cur, bool) else str(cur).lower())
        return 0
    if a.cmd == 'create':
        valid_name(a.name)
        d = idir(a.name)
        if os.path.exists(d):
            old = read(d) if os.path.isfile(os.path.join(d, 'instance.toml')) else None
            if not (a.replace and old and old.get('ephemeral')):
                raise InstanceError('%s exists%s' % (d, '' if old is None or old.get('ephemeral') else ' (not a throw-away instance: delete it first)'))
            if running(old):
                raise InstanceError('%s is running: sydb --instance %s stop' % (a.name, a.name))
            shutil.rmtree(d)
        st = {k: v for k, v in (('audio', a.audio), ('gl', a.gl)) if v}
        inst = new(a.name, a.device, a.primary, a.ephemeral, a.variant, st, a.notes)
        write(inst)
        mark(inst)
        print('instance %s: %s%s, %s' % (a.name, device_id(inst), ' (primary)' if inst['primary'] else '', d))
        return 0
    if a.cmd == 'clone':
        src = read(a.src)
        valid_name(a.new)
        d = idir(a.new)
        if os.path.exists(d):
            raise InstanceError('%s exists' % d)
        if running(src):
            raise InstanceError('%s is running: stop it first (its drives are being written)' % a.src)
        sparse_copytree(src['_dir'], d)
        inst = read(d)
        inst.update({'name': a.new, 'primary': False, 'created': datetime.date.today().isoformat(), '_dir': d})
        write(inst)
        print('instance %s: a copy of %s (%s), not primary' % (a.new, a.src, device_id(inst)))
        return 0
    if a.cmd == 'rename':
        inst = read(a.old)
        valid_name(a.new)
        if running(inst):
            raise InstanceError('%s is running: stop it first' % a.old)
        d = idir(a.new)
        if os.path.exists(d):
            raise InstanceError('%s exists' % d)
        os.rename(inst['_dir'], d)
        inst.update({'name': a.new, '_dir': d})
        write(inst)
        print('instance %s is now %s' % (a.old, a.new))
        return 0
    if a.cmd == 'delete':
        inst = read(a.name)
        if running(inst):
            raise InstanceError('%s is running: stop it first' % a.name)
        if not a.yes:
            raise InstanceError('this deletes %s with its drives and snapshots: add --yes' % inst['_dir'])
        shutil.rmtree(inst['_dir'])
        print('deleted %s' % a.name)
        return 0
    if a.cmd == 'set':
        inst = read(a.name)
        if a.key.startswith('settings.'):
            k = a.key.split('.', 1)[1]
            allowed = SETTINGS.get(k)
            if allowed is None:
                raise InstanceError('unknown setting %s (known: %s)' % (k, ', '.join(SETTINGS)))
            v = allowed(a.value) if callable(allowed) else a.value
            if isinstance(allowed, tuple) and v not in allowed:
                raise InstanceError('settings.%s: one of %s' % (k, ', '.join(allowed)))
            inst.setdefault('settings', {})[k] = v
        elif a.key == 'rom_variant':
            inst['rom_variant'] = '' if a.value in ('', '-', 'none') else a.value
            rom(inst)
        elif a.key == 'primary':
            inst['primary'] = a.value in ('1', 'true', 'yes')
            if inst['primary']:
                _unprimary(device_id(inst), inst['name'])
        elif a.key == 'firmware':
            if running(inst):
                raise InstanceError('%s is running: stop it first' % a.name)
            if a.value not in devmodel.firmware_names(inst['model']):
                raise InstanceError('%s has no firmware %s (have: %s)' % (inst['model'], a.value, ', '.join(devmodel.firmware_names(inst['model']))))
            if not (a.keep_drives or a.wipe_drives):
                raise InstanceError('another firmware may not boot this C: (first-boot state, data of the old '
                                    'version): --keep-drives to try, --wipe-drives for a factory-fresh phone')
            inst['firmware'] = a.value
            if a.wipe_drives and os.path.isdir(os.path.join(inst['_dir'], 'drives')):
                shutil.rmtree(os.path.join(inst['_dir'], 'drives'))
            write(inst)
            mark(inst)
            print('instance %s: firmware %s (%s)' % (a.name, a.value, device_id(inst)))
            return 0
        else:
            raise InstanceError('settable: settings.gl|audio|scale, rom_variant, primary, firmware')
        write(inst)
        print('instance %s: %s = %s' % (a.name, a.key, a.value))
        return 0
    if a.cmd == 'adopt':
        src = os.path.abspath(a.dir)
        if not os.path.isdir(src):
            raise InstanceError('%s is not a directory' % src)
        if os.path.isfile(os.path.join(src, 'instance.toml')):
            raise InstanceError('%s is an instance already' % src)
        m = os.path.join(src, 'device')
        dev = a.device or (open(m).read().strip() if os.path.exists(m) else None)
        if not dev:
            raise InstanceError('%s has no device mark: --device ID' % src)
        try:
            pid = int(open(os.path.join(src, 'daemon.pid')).read())
            os.kill(pid, 0)
            raise InstanceError('%s is running (daemon %d): stop it first' % (src, pid))
        except (OSError, ValueError):
            pass
        valid_name(a.name)
        d = idir(a.name)
        if os.path.exists(d):
            raise InstanceError('%s exists' % d)
        inst = new(a.name, dev, a.primary)
        os.makedirs(home(), exist_ok=True)
        os.rename(src, d)
        write(inst)
        mark(inst)
        print('instance %s: %s%s, from %s' % (a.name, device_id(inst), ' (primary)' if inst['primary'] else '', os.path.relpath(src, ROOT)))
        return 0
    if a.cmd == 'primary':
        dev = devmodel.resolve(a.device)
        inst = primary_for(dev)
        if not inst:
            raise InstanceError('%s has no primary instance (tools/instance.py create NAME %s)' % (dev, dev))
        print(inst['name'])
        return 0
    if a.cmd == 'check':
        rows = {a.name: read(a.name)} if a.name else instances(True)
        bad = 0
        prim = {}
        for n, inst in rows.items():
            errs = check_one(inst)
            if inst.get('primary') and not errs:
                prim.setdefault(device_id(inst), []).append(n)
            print('%s: %s' % (n, 'ok' if not errs else '; '.join(errs)))
            bad += bool(errs)
        for dev, ns in prim.items():
            if len(ns) > 1:
                print('error: %s has %d primary instances: %s' % (dev, len(ns), ', '.join(ns)))
                bad += 1
        return 1 if bad else 0
    return 1


if __name__ == '__main__':
    sys.exit(main())
