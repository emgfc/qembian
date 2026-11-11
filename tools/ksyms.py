"""Kernel DWARF placement for gdb: add-symbol-file arguments for our ekern (_<baseport>_ekern.exe.sym).

The ROM puts ekern's code at its ROM address with .rodata right after .text (same offsets as in the
ELF), and .data/.bss at the kernel data address. Both depend on the kernel build of the device
($SYBORG_DEVICE profile, kernel.baseport): the multiple-model build (syborg) has ekern code at
0x80003000 and data at 0xC8000000; the moving-model one (syborgmv, N73) puts the ROM at 0xF8000000,
kernel data at 0x64000000, and ekern after the firmware image. So the addresses are read from the
device's kernel-side ROM (build/roms/<id>/<fw>/kernel-side.img), whose files keep
their addresses in the composed ROM. Computed from the ELF so a rebuilt kernel needs no edits."""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb')
KSYM = os.path.join(REL, '_syborg_ekern.exe.sym')
CODE, DATA = 0x80003000, 0xc8000000          # the syborg (multiple model) build


def baseport():
    try:
        sys.path.insert(0, os.path.join(ROOT, 'tools'))
        import device
        return device.get(device.load(os.environ.get('SYBORG_DEVICE')), 'kernel.baseport', 'syborg')
    except Exception:
        return 'syborg'


def placement(bp=None):
    """(ELF, ekern code address, kernel data address) of the device's kernel build"""
    bp = bp or baseport()
    elf = os.path.join(REL, '_%s_ekern.exe.sym' % bp)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import romimg, device
    img = os.path.join(device.paths(device.load(os.environ.get('SYBORG_DEVICE')))['dir'], 'kernel-side.img')
    if not os.path.exists(img):			# not built yet: the multiple-model layout below the user side
        if bp == 'syborg':
            return elf, CODE, DATA
        raise SystemExit('ksyms: %s not built (tools/device.py build)' % img)
    rom = romimg.Rom(open(img, 'rb').read())
    for f in rom.walk():
        if f['path'].lower().endswith('ekern.exe'):
            return elf, rom.image(f['addr'])['code'], rom.kerndata
    raise SystemExit('ksyms: no ekern.exe in %s' % img)


def sections(path=KSYM):
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(r'\]\s+(\.\w+)\s+\w+\s+([0-9a-f]+)', out)}


def add_symbol_file(path=None):
    elf, code, data = placement()
    if path and os.path.basename(path) != os.path.basename(elf):
        elf, code, data = path, CODE, DATA     # an explicit multiple-model ELF (tools/syb_gdb.py)
    s = sections(elf)
    return 'add-symbol-file %s -s .text 0x%x -s .rodata 0x%x -s .data 0x%x -s .bss 0x%x' % (
        elf, code, code + s['.rodata'] - s['.text'], data, data + s['.bss'] - s['.data'])


if __name__ == '__main__':
    print(add_symbol_file())
