#!/usr/bin/env python3
"""A device bundle for the Android app (android/README.md): everything one phone needs, made on the PC.

  python3 android/tools/make_bundle.py DEVICE_ID [--out DIR] [--card-mb N]
  -> DIR (default build/android/bundles/DEVICE_ID)/
       qemu-args.txt   one QEMU argument per line; @BUNDLE@ = this directory on the phone, @DATA@ = the
                       app's writable directory for this device (drives, logs)
       bundle.json     id, name, screen size, the front keys for the app's key strip, drive images
       hybrid.img, hybrid.img.rofs, syborg-device.dtb, keymap.qemu, ... (the files the arguments name)
       nvmemory.img    the C: drive as a new phone starts (the app copies it to @DATA@ once)
Push it: adb push DIR /sdcard/Android/data/org.qembian.phone/files/bundles/
The app knows no device: everything phone-specific comes from here (an N8 is `make_bundle.py nokia-n8-rm596`).
"""
import argparse, json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel   # tools/device.py


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('device')
    ap.add_argument('--out')
    ap.add_argument('--card-mb', type=int, default=1024, help='memory card (E:) size the app creates (MB)')
    a = ap.parse_args()
    os.environ['SYBORG_DEVICE'] = a.device
    prof = devmodel.load(a.device)
    p = devmodel.paths(prof)
    rom = p['rom']
    if not os.path.exists(rom):
        raise SystemExit('no ROM for %s: tools/device.py build %s' % (a.device, a.device))
    out = os.path.abspath(a.out or os.path.join(ROOT, 'build', 'android', 'bundles', a.device))
    os.makedirs(out, exist_ok=True)

    args = devmodel.qemu_args(prof, rom)
    names = {}

    def take(path):                     # a file an argument names: into the bundle, the path becomes @BUNDLE@/name
        path = os.path.abspath(path)
        name = os.path.basename(path)
        if names.get(name, path) != path:
            name = '%d-%s' % (len(names), name)
        names[name] = path
        dst = os.path.join(out, name)
        if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(path) or os.path.getsize(dst) != os.path.getsize(path):
            shutil.copy2(path, dst)
        return '@BUNDLE@/' + name

    fixed = []
    for x in args:
        if x.startswith('/') and os.path.isfile(x):
            x = take(x)
        elif '=' in x:                  # -global dev.prop=/path
            k, _, v = x.partition('=')
            if v.startswith('/') and os.path.isfile(v):
                x = k + '=' + take(v)
        fixed.append(x)
    # the ROM and its device tree (scripts/run-modern.sh adds these on the desktop)
    fixed += ['-kernel', take(rom), '-dtb', take(p['dtb'])]
    # the C: drive as a new phone starts (the firmware's user data layer, or blank at the profile's size)
    nv = os.path.join(out, 'nvmemory.img')
    if not os.path.exists(nv):
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'device.py'), 'drive-image', a.device,
                               '--kind', 'nvmem', '--out', nv])
    # Android host side: no display (the app's window), the GL thread presenting into the window, serial and
    # drives in the app's storage, no audio yet, a bounded JIT cache
    fixed += ['-display', 'none', '-monitor', 'none', '-serial', 'file:@DATA@/serial.log',
              '-drive', 'if=mtd,format=raw,file=@DATA@/nvmemory.img',
              '-drive', 'if=mtd,index=1,format=raw,file=@DATA@/nvmemory2.img',
              '-global', 'syborg-gl.backend=host', '-global', 'syborg-gl.thread=on',
              '-global', 'syborg-gl.present=window', '-global', 'syborg-gl.msaa=4',
              '-audiodev', 'none,id=snd0', '-machine', 'audiodev=snd0',
              '-accel', 'tcg,tb-size=256',
              '-global', 'syborg-rtc.offset-file=@DATA@/rtc-offset']
    # msaa: the profile's value is replaced (Adreno: 4 samples at most); drop the earlier -global for it
    out_args, i = [], 0
    while i < len(fixed):
        if fixed[i] == '-global' and i + 1 < len(fixed) and fixed[i + 1].startswith('syborg-gl.msaa=') and \
                fixed[i + 1] != 'syborg-gl.msaa=4':
            i += 2
            continue
        if fixed[i] == '-nic':          # no slirp in the Android build yet: syborg-net stays without a backend
            i += 2
            continue
        out_args.append(fixed[i])
        i += 1
    open(os.path.join(out, 'qemu-args.txt'), 'w').write('\n'.join(out_args) + '\n')

    disp = devmodel.get(prof, 'board.display', {})
    keys = []
    try:
        import tomllib
        km = tomllib.load(open(devmodel.device_file(prof, 'keymap.toml'), 'rb'))
    except Exception:
        km = {}
    for k in km.get('keys', []):
        host = (k.get('host') or [None])[0]
        if host:
            keys.append({'key': k['key'], 'label': k.get('label', k['key']), 'qcode': host,
                         'group': k.get('group', ''), 'style': k.get('style', ''), 'col': k.get('col', 0)})
    info = {'id': a.device, 'name': prof.get('name', a.device), 'width': disp.get('width', 240),
            'height': disp.get('height', 320), 'keys': keys,
            'drives': [{'file': 'nvmemory.img', 'from': 'nvmemory.img'},
                       {'file': 'nvmemory2.img', 'size_mb': a.card_mb}]}
    json.dump(info, open(os.path.join(out, 'bundle.json'), 'w'), indent=1)
    total = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print('%s: %d files, %.0f MB in %s' % (a.device, len(os.listdir(out)), total / 1e6, out))


if __name__ == '__main__':
    main()
