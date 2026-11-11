#!/usr/bin/env python3
"""Derive another Syborg baseport variant from the patched one (docs/devices.md §4: one kernel build
per memory model). The copy shares every source with baseport/syborg; only the build knobs differ:
variant prefix (_<name>_ekern.exe ...), memory model, CPU, VFP, the ROM header (linear base, kernel
data address) and the export directory of the ROM includes.

  tools/baseport_variant.py NAME --memmodel moving --cpu arm926 [--epocroot build/epocroot]

Known variants (kernels/*.toml would be the next step if there are more):
  syborgmv   moving memory model, ARM926EJ-S, no VFP: Symbian 9.1 phones (N73: OMAP1710), whose
             ROMs are linked at 0xF8000000 and use moving-model fixed processes
  syborgfx   flexible memory model, ARM1176 (ARMv6K), VFP: Symbian^3 phones (N8). Their ROMs put DLL data
             just under 0x7f000000 (the flexible model's local data end; probe.py header tells), and EUSER
             keeps per-thread data in the user thread-ID register TPIDRURW (the kernel then switches it)
  syborg6k   multiple memory model, ARM1176 (ARMv6K), VFP: Symbian 9.3 RAPIDO phones whose EUSER also keeps
             per-thread data in TPIDRURW (E52, E72, E5-00, 6720 classic: KERN-EXEC 3 at 1.5 s on ARM1136 r0)
Re-run after every change to baseport/syborg (scripts/setup-epocroot.sh does it after the patches).
"""
import argparse, os, re, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CPUS = {   # config.inc symbol, variant.mmh macro, has VFP
    'arm926':  ('CFG_CPU_ARM926J', '__CPU_ARM926J__', False),
    'arm1136': ('CFG_CPU_ARM1136', '__CPU_ARM1136__', True),
    'arm1176': ('CFG_CPU_ARM1176', '__CPU_ARM1176__', True),   # ARMv6K: user thread-ID registers (Symbian^3 EUSER)
}
MODELS = {  # rombuild memmodel line, ROM linear base, kernel data address (mmboot.h of the model)
    'moving':   ('memmodel moving 0x100000 0x1000', 0xF8000000, 0x64000000),
    'multiple': ('memmodel multiple 0x100000', 0x80000000, 0xC8000000),
    'flexible': ('memmodel flexible 0x100000 0x1000 -0x4000', 0x80000000, 0xC8000000),   # Symbian^3 (N8)
}
SKIP = {'standins', 'n95compat'}           # per-device, copied in by scripts/build-standins.sh


def sub(path, pairs):
    s = open(path, newline='').read()
    for old, new in pairs:
        if isinstance(old, re.Pattern):
            s, n = old.subn(new, s)
        else:
            n = s.count(old)
            s = s.replace(old, new)
        if not n:
            raise SystemExit('%s: %r not found (baseport/syborg changed? update tools/baseport_variant.py)'
                             % (os.path.relpath(path, ROOT), old.pattern if isinstance(old, re.Pattern) else old))
    open(path, 'w', newline='').write(s)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('name')
    ap.add_argument('--memmodel', choices=MODELS, required=True)
    ap.add_argument('--cpu', choices=CPUS, required=True)
    ap.add_argument('--epocroot', default=os.path.join(ROOT, 'build', 'epocroot'))
    ap.add_argument('--unaligned', action='store_true',
                    help='no alignment checking (SCTLR.A clear, U set): user code may load and store '
                         'unaligned words, as Symbian^3 phones\' code does (RVCT for ARMv6)')
    a = ap.parse_args()
    base = os.path.join(a.epocroot, 'sf', 'adapt', 'qemu', 'baseport')
    src, dst = os.path.join(base, 'syborg'), os.path.join(base, a.name)
    if a.name == 'syborg':
        raise SystemExit('the source variant itself')
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True, ignore=lambda d, names: [n for n in names if d == src and
                                                                      (n in SKIP or n.endswith('.tmp'))])
    cfg, macro, vfp = CPUS[a.cpu]
    mm, rombase, kdata = MODELS[a.memmodel]
    other = [m for m in MODELS if m != a.memmodel]
    # variant.mmh: prefix, CPU, VFP, memory model and its include paths
    vm = [('_syborg_##name', '_%s_##name' % a.name),
          (re.compile(r'^macro __CPU_ARM1136__\s*$', re.M), 'macro %s' % macro),
          (re.compile(r'^#define MM_MULTIPLE\s*$', re.M), '//#define MM_MULTIPLE'),
          (re.compile(r'^//#define MM_%s\s*$' % a.memmodel.upper(), re.M), '#define MM_%s' % a.memmodel.upper())]
    if a.memmodel != 'multiple':
        vm += [('/memmodel/epoc/multiple', '/memmodel/epoc/%s' % a.memmodel),
               ('(memmodel/epoc/multiple', '(memmodel/epoc/%s' % a.memmodel)]
    if not vfp:
        vm += [(re.compile(r'^macro __CPU_HAS_VFP\s*$', re.M), '//macro __CPU_HAS_VFP  (%s: no VFP)' % a.cpu)]
    sub(os.path.join(dst, 'variant.mmh'), vm)
    # config.inc: bootstrap CPU
    ci = [(re.compile(r'^\tGBLL\tCFG_CPU_ARM1136\b.*$', re.M), '\tGBLL\t%s' % cfg)]
    if a.unaligned:
        if a.cpu != 'arm1176':
            raise SystemExit('--unaligned: only the ARM1176 control register value is known here')
        # bootcpu.inc's ARM1176 InitialMMUCR without MMUCR_A (INIT_NUMERIC_CONSTANT keeps ours);
        # ExtraMMUCR there already sets MMUCR_U
        ci.append((re.compile(r'^(\tGBLL\t%s.*)$' % cfg, re.M),
                   '\\1\n; tools/baseport_variant.py --unaligned: no alignment checking (Symbian^3 user code)\n'
                   'InitialMMUCR\tEQU\tMMUCR_W+MMUCR_SBO+MMUCR_I+MMUCR_IT+MMUCR_DT+MMUCR_TRE+MMUCR_FA'))
    sub(os.path.join(dst, 'config.inc'), ci)
    # bld.inf: config/bootstrap prefixes, bootstrap memory model, ROM include exports, tshell ROM target
    sub(os.path.join(dst, 'bld.inf'), [
        ('_syborg_', '_%s_' % a.name),
        (re.compile(r'^option\t\tMEMMODEL\t\tmultiple\s*$', re.M), 'option\t\tMEMMODEL\t\t%s' % a.memmodel),
        ('/epoc32/rom/syborg/', '/epoc32/rom/%s/' % a.name),
        (re.compile(r'REE_TARGET\tsyborg\b'), 'REE_TARGET\t%s' % a.name)])
    # ROM header: model, addresses, bootstrap binary
    sub(os.path.join(dst, 'rom', 'header.iby'), [
        (re.compile(r'^memmodel multiple 0x100000\s*$', re.M), mm),
        ('_syborg_bootloader_bootrom.bin', '_%s_bootloader_bootrom.bin' % a.name),
        (re.compile(r'^romlinearbase=0x80000000', re.M), 'romlinearbase=0x%08X' % rombase),
        (re.compile(r'^kerneldataaddress=0xC8000000', re.M), 'kerneldataaddress=0x%08X' % kdata),
        (re.compile(r'^#define VARIANT_PATH syborg\s*$', re.M), '#define VARIANT_PATH %s' % a.name)])
    open(os.path.join(dst, 'VARIANT.txt'), 'w').write(
        'generated by tools/baseport_variant.py %s --memmodel %s --cpu %s%s from baseport/syborg; do not edit\n'
        % (a.name, a.memmodel, a.cpu, ' --unaligned' if a.unaligned else ''))
    print('baseport/%s: %s memory model, %s, ROM at 0x%08X, kernel data 0x%08X' % (a.name, a.memmodel, a.cpu, rombase, kdata))
    return 0


if __name__ == '__main__':
    sys.exit(main())
