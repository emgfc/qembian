"""Kernel DWARF placement for gdb: add-symbol-file arguments for _syborg_ekern.exe.sym.

The ROM puts ekern's code at 0x80003000 with .rodata right after .text (same offsets as in the
ELF), and .data/.bss at 0xc8000000 (kernel data). Computed from the ELF so a rebuilt kernel needs
no edits (the old hardcoded .rodata address went stale with the ARM1136 build)."""
import os, re, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KSYM = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb', '_syborg_ekern.exe.sym')
CODE, DATA = 0x80003000, 0xc8000000


def sections(path=KSYM):
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(r'\]\s+(\.\w+)\s+\w+\s+([0-9a-f]+)', out)}


def add_symbol_file(path=KSYM):
    s = sections(path)
    return 'add-symbol-file %s -s .text 0x%x -s .rodata 0x%x -s .data 0x%x -s .bss 0x%x' % (
        path, CODE, CODE + s['.rodata'] - s['.text'], DATA, DATA + s['.bss'] - s['.data'])


if __name__ == '__main__':
    print(add_symbol_file())
