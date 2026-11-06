#!/usr/bin/env python3
"""device.py: the device model (docs/devices.md). Loads devices/<id>/device.toml (with `extends`
inheritance) and answers the tools that need device facts, so no script carries an N95 literal.

  tools/device.py list                         known devices (id, model, level)
  tools/device.py check [ID]                   schema, inheritance, files, keymap names/collisions
  tools/device.py show [ID] [--json]           the merged profile
  tools/device.py get [ID] KEY                 one value by dotted key (board.ram_mb)
  tools/device.py paths [ID] [NAME]            build/firmware paths (dir, core_flat, rom, dtb, ...)
  tools/device.py qemu-args [ID] [--rom ROM]   board arguments for the one QEMU binary
  tools/device.py gen keymap-header [ID]       the guest EKEYB table (KConvertCode lines)
  tools/device.py gen key-aliases [ID]         sydb key name -> QEMU qcode, as JSON
  tools/device.py gen standins-bldinf [ID]     bld.inf of the device's kernel-side stand-ins (src/standins)
  tools/device.py gen standins-oby [ID]        "kind name ext" per stand-in, in ROM order
  tools/device.py build [ID] [--variant V] [--kernel] [--no-standins] [--out IMG]
                                               compose the device ROM (replaces hybrid-boot.sh
                                               --compose-only / compose-exp.sh)

ID defaults to $SYBORG_DEVICE, else devices/default (a symlink); aliases from [device].aliases work.
Stdlib only (tomllib), so the python3 -I tools can import it.
"""
import argparse, json, os, re, sys, tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVICES = os.path.join(ROOT, 'devices')
STDKEYS = os.path.join(ROOT, 'platform', 'symbian', 'stdkeys.toml')


class DeviceError(Exception):
    pass


# ---------------------------------------------------------------- loading and inheritance

def device_ids():
    return sorted(d for d in os.listdir(DEVICES)
                  if os.path.isfile(os.path.join(DEVICES, d, 'device.toml')) and not os.path.islink(os.path.join(DEVICES, d)))


def resolve(dev=None):
    dev = dev or os.environ.get('SYBORG_DEVICE') or 'default'
    p = os.path.join(DEVICES, dev)
    if os.path.isfile(os.path.join(p, 'device.toml')):
        return os.path.basename(os.path.realpath(p))
    for d in device_ids():
        prof = _read(d)
        if dev in prof.get('device', {}).get('aliases', []):
            return d
    raise DeviceError('unknown device %r (devices: %s)' % (dev, ', '.join(device_ids())))


def _read(dev):
    with open(os.path.join(DEVICES, dev, 'device.toml'), 'rb') as f:
        return tomllib.load(f)


def _merge(base, over):
    """tables deep-merge; scalars and plain arrays replace; arrays of tables merge by `name`
    (or `key`/`letter`/`role`), with an optional `remove = [names]` beside them"""
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
        for k, v in list(out.items()):
            if isinstance(v, list):
                out[k] = [x for x in v if not (isinstance(x, dict) and gone in (x.get('name'), x.get('key')))]
    return out


def load(dev=None, _seen=None):
    dev = resolve(dev)
    seen = _seen or []
    if dev in seen:
        raise DeviceError('extends loop: %s' % ' -> '.join(seen + [dev]))
    prof = _read(dev)
    parent = prof.get('extends')
    if parent:
        prof = _merge(load(parent, seen + [dev]), prof)
        prof.pop('extends', None)
        prof['_extends'] = parent
    prof['_id'] = dev
    prof['_dir'] = os.path.join(DEVICES, dev)
    return prof


def get(prof, key, default=None):
    v = prof
    for part in key.split('.'):
        if not isinstance(v, dict) or part not in v:
            return default
        v = v[part]
    return v


def device_file(prof, name):
    """a file named in the profile: in the device's own directory, else in the parent's"""
    d = prof['_id']
    while d:
        p = os.path.join(DEVICES, d, name)
        if os.path.exists(p):
            return p
        d = _read(d).get('extends')
    return os.path.join(prof['_dir'], name)


def paths(prof):
    """build and firmware locations (build/n95 is the N95's historical name, kept as is)"""
    bdir = os.path.join(ROOT, 'build', 'devices', prof['_id'])
    if not os.path.exists(bdir) and prof['_id'] == 'nokia-n95-8gb-rm320':
        bdir = os.path.join(ROOT, 'build', 'n95')
    return {
        'dir': bdir,
        'core_flat': os.path.join(bdir, 'core-flat.img'),
        'core_json': os.path.join(bdir, 'core.json'),
        'rofs': [os.path.join(bdir, 'rofs1.img'), os.path.join(bdir, 'rofs2.img')],
        'rom': os.path.join(bdir, 'hybrid.img'),
        'manifest': os.path.join(bdir, get(prof, 'rom.manifest', 'hybrid-manifest.txt')),
        'dtb': os.path.join(bdir, 'syborg-n95.dtb' if prof['_id'] == 'nokia-n95-8gb-rm320' else 'syborg-device.dtb'),
        'symindex': os.path.join(bdir, 'symindex.json'),
        'firmware': os.path.join(ROOT, 'firmware', prof['_id']),
    }


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
    """one "obykind name ext" line per stand-in: ldd/pdd are device[], extensions extension[] .pdd"""
    out = []
    for s in prof.get('standins', []):
        k = s['kind']
        out.append({'ldd': 'device %s ldd', 'pdd': 'device %s pdd', 'extension': 'extension %s pdd'}[k] % s['name'])
    return out


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


def build(prof, variant=None, kernel=False, standins=True, out=None, trace=None):
    """the ROM pipeline of scripts/hybrid-boot.sh, every argument from the profile"""
    ps = paths(prof)
    logs = os.path.join(ROOT, 'build', 'logs')
    os.makedirs(logs, exist_ok=True)
    env_dev = {'SYBORG_DEVICE': prof['_id']}
    os.environ.update(env_dev)
    k = os.path.join('build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'kernel', 'eka')
    pers = get(prof, 'platform.personality')
    if pers == 'sym92':			# exec numbering table compiled into the kernel (personalities, step 4)
        _run(['python3', '-I', 'tools/exec92/gen_kernel_map.py', k + '/kernel/execs.txt',
              'tools/exec92/exec92.map', k + '/kernel/exec92_map.h'])
    if kernel:
        _run(['scripts/build-syborg.sh', get(prof, 'kernel.variant', 'udeb')], os.path.join(logs, 'device-syborg-build.txt'))
    if standins:
        _run(['scripts/build-standins.sh', get(prof, 'kernel.variant', 'udeb'), prof['_id']],
             os.path.join(logs, 'device-standins-build.txt'))
    var = get(prof, 'rom.variants.%s' % variant) if variant else None
    if variant and var is None:
        raise DeviceError('%s has no ROM variant %r' % (prof['_id'], variant))
    img = out or os.path.join(ps['dir'], var.get('image', 'hybrid-%s.img' % variant) if var else 'hybrid.img')
    # code patches (variants omit patches by hack tag, "[H-...]" in the line)
    omit = var.get('omit_patches', []) if var else []
    pfiles = []
    for name in get(prof, 'rom.code_patches', []):
        src = device_file(prof, name)
        if omit:
            lines = [l for l in open(src, newline='').read().split('\n')
                     if not any('[%s]' % h in l for h in omit)]
            dst = os.path.join(ROOT, 'build', 'standins-gen', 'code-patches-%s.txt' % variant)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'w', newline='').write('\n'.join(lines))
            src = dst
        pfiles.append(src)
    vr = get(prof, 'kernel.variant', 'udeb')
    cmd = ['python3', '-I', 'tools/romcompose.py',
           '--ours', 'build/rom-hybrid/syborg_hybrid_kernel_ARMV5_%s.img' % vr,
           '--n95', ps['core_flat'], '-o', img, '--manifest', ps['manifest'],
           '--trace', trace or hex(get(prof, 'kernel.trace_mask', 0x80000000))]
    if pers == 'sym92':
        cmd.append('--exec92')
    for f in get(prof, 'rom.files', []):
        cmd += ['--file', '%s=%s' % (f['rom'], device_file(prof, f['local']))]
    for r in ps['rofs']:
        if os.path.exists(r):
            cmd += ['--rofs', r]
    for pf in pfiles:
        cmd += ['--patches', pf]
    blob = os.path.join(ROOT, 'build', 'n95gl', 'sygl.bin')
    if get(prof, 'gl.impl') == 'sygl' and os.path.exists(blob) and os.environ.get('SYGL', '1') == '1':
        addr = open(os.path.join(ROOT, 'build', 'n95gl', 'sygl.addr')).read().strip()
        cmd += ['--patches', device_file(prof, get(prof, 'gl.entry_patches', 'gl/gles-patches.txt')),
                '--blob', '%s=%s' % (addr, blob)]
    _run(cmd, os.path.join(logs, 'device-compose.txt'))
    disp = get(prof, 'board.display', {})
    _run(['python3', '-I', 'tools/dtb_set.py', 'external/oss.FCL.sf.adapt.Qemu/baseport/syborg/syborg.dtb', ps['dtb'],
          'framebuffer:width=%d' % disp.get('width', 240), 'framebuffer:height=%d' % disp.get('height', 320)],
         os.path.join(logs, 'device-dtb.txt'))
    return img


# ---------------------------------------------------------------- QEMU

def qemu_args(prof, rom=None):
    b = prof.get('board', {})
    disp = b.get('display', {})
    a = ['-M', b.get('machine', 'syborg'), '-cpu', b.get('cpu', 'arm1136-r2'),
         '-m', '%dM' % b.get('ram_mb', 168),
         '-global', 'syborg-fb.width=%d' % disp.get('width', 240),
         '-global', 'syborg-fb.height=%d' % disp.get('height', 320)]
    if 'refresh_hz' in disp:
        a += ['-global', 'syborg-fb.refresh-rate=%d' % disp['refresh_hz']]
    if 'blank_dim_percent' in disp:
        a += ['-global', 'syborg-fb.blank-dim=%d' % disp['blank_dim_percent']]
    fl = b.get('flash')
    if rom and fl and os.path.exists(rom + '.rofs'):
        a += ['-device', 'loader,file=%s.rofs,addr=0x%x,force-raw=on' % (rom, fl['base'])]
    return a


def drive_sizes(prof):
    return {d['kind']: d.get('size') for d in get(prof, 'board.drives', []) if d.get('image')}


# ---------------------------------------------------------------- check

def check(prof):
    errs, warn = [], []
    for k in ('device', 'platform', 'board', 'kernel'):
        if k not in prof:
            errs.append('missing [%s]' % k)
    if get(prof, 'device.id') != prof['_id']:
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
    if not os.path.isdir(os.path.join(ROOT, 'personalities', get(prof, 'platform.personality', ''))):
        warn.append('personality %r has no directory yet (refactor step 4)' % get(prof, 'platform.personality'))
    return errs, warn


# ---------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
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
    p = sub.add_parser('build'); p.add_argument('dev', nargs='?'); p.add_argument('--variant')
    p.add_argument('--kernel', action='store_true', help='also rebuild the Syborg baseport/kernel side')
    p.add_argument('--no-standins', action='store_true'); p.add_argument('--out'); p.add_argument('--trace')
    p = sub.add_parser('gen'); p.add_argument('what', choices=['keymap-header', 'key-aliases', 'standins-bldinf', 'standins-oby'])
    p.add_argument('dev', nargs='?')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'list':
            for d in device_ids():
                pr = load(d)
                print('%-28s %-24s %s' % (d, '%s %s' % (get(pr, 'device.vendor', ''), get(pr, 'device.model', '')),
                                         get(pr, 'status.level', '')))
            return 0
        if a.cmd == 'get':
            dev, key = (a.args[0], a.args[1]) if len(a.args) > 1 else (None, a.args[0])
            v = get(load(dev), key)
            if v is None:
                print('no %s' % key, file=sys.stderr)
                return 1
            print(json.dumps(v) if isinstance(v, (dict, list)) else v)
            return 0
        if a.cmd == 'paths' and a.dev and not a.name and a.dev in ('dir', 'core_flat', 'core_json', 'rofs', 'rom', 'manifest',
                                                                 'dtb', 'symindex', 'firmware'):
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
            ps = paths(prof)
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
        if a.cmd == 'build':
            img = build(prof, a.variant, a.kernel, not a.no_standins, a.out, a.trace)
            print(open(os.path.join(ROOT, 'build', 'logs', 'device-compose.txt')).read().strip().splitlines()[-1])
            print('ROM: %s' % img)
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
