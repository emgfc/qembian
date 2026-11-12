#!/usr/bin/env python3
"""htest: build and run the hosted-EKA2 kernel tests on the QEMU aarch64 `virt` machine.

The tests are 32-bit ARM Linux programs (src/hosted/tests/t_*.c, built freestanding on the hrt runtime in
src/hosted/rt with clang + the aarch64 binutils' armelf linker); an aarch64 init (src/hosted/init) runs
them in the guest and prints HTEST-* lines; this tool packs an initramfs, boots
build/hosted/linux (scripts/build-hosted-kernel.sh) on build/hosted/qemu (scripts/build-hosted-qemu.sh),
parses the console and exits non-zero when a test fails. docs/hosted-eka2/tests.md.

  python3 tools/hosted/htest.py                 # build all, run all
  python3 tools/hosted/htest.py t_svc t_tls     # only these
  python3 tools/hosted/htest.py --build-only
  python3 tools/hosted/htest.py -v              # the whole console
  python3 tools/hosted/htest.py -j 4            # the tests split over 4 QEMUs in parallel
  python3 tools/hosted/htest.py --matrix -j 2   # every machine config (1/2/4 CPUs, a55/max) in parallel
  options: --cpu cortex-a55|max  --smp N  --timeout S (per test)  --cp15 0|1|2  --append 'k=v ...'
"""
import argparse
import json
import glob
import os
import re
import shutil
import stat
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'src/hosted')
OUT = os.path.join(ROOT, 'build/hosted/htest')
QEMU = os.path.join(ROOT, 'build/hosted/qemu/install/bin/qemu-system-aarch64')
KERNEL = os.path.join(ROOT, 'build/hosted/linux/arch/arm64/boot/Image')

EKA = os.path.join(ROOT, 'build/epocroot/sf/os/kernelhwsrv/kernel/eka')
EINC = os.path.join(ROOT, 'build/epocroot/epoc32/include')
NKLINUX = os.path.join(SRC, 'nkern', 'linux')
# the unmodified generic EKA2 nanokernel (EPL tree, scripts/setup-epocroot.sh) + our Linux platform layer
NKERN_GENERIC = ['nkern', 'nklib', 'nkerns', 'sched', 'dfcs', 'nk_timer']
# built-in kernel modules: (name for the renamed symbols, ROM file name it replaces, sources (relative to
# the EKA2 tree, or absolute)); the hosted kernel uses them where the ROM has the ARM image of the module
MODEM_CORE_DIR = os.path.join(ROOT, 'src/qemu-syborg/hw/arm')   # syborg_modem_core.c/.h (QEMU and hosted)
# the baseport's copies of our stand-ins (src/standins/<name>, relative includes resolve there)
STANDINS = os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/standins') + '/'
KMODULES = [
    ('Exstart', 'EXSTART.DLL', ['kernel/exstart.cpp']),
    ('Elocd', 'ELOCD.LDD', ['drivers/locmedia/locmedia.cpp', 'drivers/locmedia/dmasupport.cpp']),
    ('Medint', 'MEDINT.PDD', ['drivers/medint/iram.cpp']),
    ('Medrofs', 'medrofs.pdd', [os.path.join(ROOT, 'src/standins/medrofs/medrofs.cpp')]),
    ('Mednvmem', 'MEDNVMEMORY.PDD', [os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/mednvmemory/syborg_mednvmemory.cpp')]),
    ('Mednvmem2', 'mednvmem2.pdd', [STANDINS + 'mednvmem2/mednvmem2.cpp']),
    # the Nokia ISC stack: our stand-ins (iscpeer.inl forwards to the board's fake modem)
    ('Xbus', 'xbus.ldd', [STANDINS + 'xbus/xbus.cpp']),
    ('Iscdrv', 'nokiaiscdriver.ldd', [STANDINS + 'nokiaiscdriver/nokiaiscdriver.cpp']),
    ('Iscmux', 'iscisimultiplexer.ldd', [STANDINS + 'iscisimultiplexer/iscisimultiplexer.cpp']),
    ('Iscext', 'nokiaiscdriverextension.ldd', [STANDINS + 'nokiaiscdriverextension/nokiaiscdriverextension.cpp']),
    # the other stand-ins the N95 loads (sources from their .mmp)
    ('Custom', 'custom.ldd', STANDINS + 'custom/custom.mmp'),
    ('Sosadl', 'sosadlserver.ldd', STANDINS + 'sosadlserver/sosadlserver.mmp'),
    ('Ddisplay', 'd_display.ldd', STANDINS + 'd_display/d_display.mmp'),
    ('Secldd', 'secldd.ldd', STANDINS + 'secldd/secldd.mmp'),
    ('Ehwa', 'ehwa.ldd', STANDINS + 'ehwa/ehwa.mmp'),
    ('Ehwaomap', 'ehwaomap.pdd', STANDINS + 'ehwaomap/ehwaomap.mmp'),
    ('Dvfs', 'dvfs_interface.ldd', STANDINS + 'dvfs_interface/dvfs_interface.mmp'),
    ('Mobcrash', 'mobilecrashdriver.ldd', STANDINS + 'mobilecrashdriver/mobilecrashdriver.mmp'),
    ('Lightldd', 'lightldd.ldd', STANDINS + 'lightldd/lightldd.mmp'),
    ('Lightpdd', 'lightpdd.pdd', STANDINS + 'lightpdd/lightpdd.mmp'),
    # EPL drivers
    ('Pipelib', 'PIPELIB.LDD', 'drivers/pipe/pipe.mmp'),
    ('Ecomm', 'ECOMM.LDD', 'drivers/ecomm/ecomm.mmp'),
    ('Ekeyb', 'EKEYB.DLL', os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/keyboard/keyboard.mmp')),
    ('Epointer', 'EPOINTER.DLL', os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/pointer/pointer.mmp')),
    ('Euart', 'EUART1.PDD', os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/serial/serial.mmp')),
    # the board's display: LCD.DLL (screen HAL, video RAM) on the framebuffer model; which build is the
    # device's ([kernel.lcd] target: lcd_n95 for the N95's native-landscape panel), see device_kmodules
    ('Lcd', 'LCD.DLL', 'device:kernel.lcd'),
]


def mmp_sources(mmp):
    """the source files of a Symbian .mmp (SOURCEPATH/SOURCE lines, relative to the .mmp)"""
    mmp = mmp if os.path.isabs(mmp) else os.path.join(EKA, mmp)
    here, out = os.path.dirname(mmp), []
    path = here
    for line in open(mmp, errors='ignore'):
        w = line.split('//')[0].split()
        if len(w) >= 2 and w[0].lower() == 'sourcepath':
            path = os.path.normpath(os.path.join(here, w[1]))
        elif len(w) >= 2 and w[0].lower() == 'source':
            out += [os.path.join(path, x) for x in w[1:]]
        elif len(w) >= 2 and w[0].lower() == 'userinclude':
            MMP_INCLUDES.setdefault(mmp, []).extend(os.path.normpath(os.path.join(here, x)) for x in w[1:])
        elif len(w) >= 2 and w[0].lower() == 'macro':
            MMP_MACROS.setdefault(mmp, []).extend(w[1:])
    return out


MMP_MACROS = {}        # .mmp -> its MACRO definitions
SRC_MACROS = {}        # source -> -D flags from its module's .mmp
DEVICE = None          # the device the kernel's per-device modules are built for (htest --rom)


def device_get(key):
    """a value of the device profile (tools/device.py get), or None"""
    if not DEVICE:
        return None
    try:
        return subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'get', key],
                              env=dict(os.environ, SYBORG_DEVICE=DEVICE), capture_output=True, text=True,
                              check=True).stdout.strip() or None
    except subprocess.CalledProcessError:
        return None


def device_kmodules():
    """KMODULES with the per-device entries resolved from the profile ('device:KEY')"""
    out = []
    for mname, romname, msrcs in KMODULES:
        if isinstance(msrcs, str) and msrcs == 'device:kernel.lcd':
            target = device_get('kernel.lcd.target')
            mmp = os.path.join(STANDINS, 'lcd', '%s.mmp' % target) if target else None
            msrcs = mmp if mmp and os.path.exists(mmp) else \
                os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/fb/fb.mmp')
        out.append((mname, romname, msrcs))
    return out


MMP_INCLUDES = {}      # .mmp -> its USERINCLUDE dirs (mmp_sources fills it)

# EPL sources that clang does not accept as they are: compiled from a generated copy with the same meaning
# (source -> [(text, replacement)]). Each replacement must be what the phone's compiler made of it.
SOURCE_FIXES = {
    # `a1<0` on a TAny*: GCC/RVCT compare pointers unsigned, never true; clang refuses ordered
    # pointer/zero comparisons
    'drivers/ecomm/d_comm.cpp': [('if (a1<0)', 'if ((TUint)a1<0u)')],
    'kernel/stimer.cpp': [
        ('void msCallBack(TAny* aPtr)', 'TUint32 DiagMs, DiagMsAdded, DiagTickDfc, DiagSetTime;\nvoid msCallBack(TAny* aPtr)'),
        ('''	TTickQ& q=*(TTickQ*)aPtr;
	q.iTickDfc.Add();''', '''	TTickQ& q=*(TTickQ*)aPtr;
	TBool added = q.iTickDfc.Add();
	extern TUint32 DiagMs, DiagMsAdded; ++DiagMs; DiagMsAdded += added;'''),
        ('''	TTickQ& q=*(TTickQ*)aPtr;
	__ASSERT_DEBUG(q.iMsTimer.i_NTimer_iState==NTimer::EIdle, *(TInt*)0xdfc01bad=0);''', '''	TTickQ& q=*(TTickQ*)aPtr;
	extern TUint32 DiagTickDfc; ++DiagTickDfc;
	__ASSERT_DEBUG(q.iMsTimer.i_NTimer_iState==NTimer::EIdle, *(TInt*)0xdfc01bad=0);'''),
        ('''		tq.iMsTimer.iTriggerTime=tq.iLastMs;
		tq.iTickDfc.Cancel();''', '''		tq.iMsTimer.iTriggerTime=tq.iLastMs;
		{ extern TUint32 DiagSetTime; ++DiagSetTime; }
		tq.iTickDfc.Cancel();'''),
        ],
}


def fixed_source(src):
    """src itself, or a generated copy with SOURCE_FIXES applied (OUT/fixed/...)"""
    rel = os.path.relpath(src, EKA)
    fixes = SOURCE_FIXES.get(rel)
    if not fixes:
        return src
    out = os.path.join(OUT, 'fixed', rel)
    text = open(src).read()
    for a, b in fixes:
        if a not in text:
            sys.exit('htest: source fix %r no longer applies to %s' % (a, src))
        text = text.replace(a, b)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if not os.path.exists(out) or open(out).read() != text:
        open(out, 'w').write(text)
    return out
SRC_INCLUDES = {}      # source -> extra include dirs (from its module's .mmp)
# entries: (module name, ROM file name, sources)

# the unmodified generic EKA2 kernel layer (kernel/ekern.mmp's generic sources, klib, common)
KERNEL_GENERIC = ['kernel/' + n for n in (
    'sglobals', 'device', 'eventq', 'sexec', 'sinit', 'skernel', 'sexecpers', 'scodeseg', 'sipc', 'notifier',
    'object', 'power', 'server', 'sha256', 'securerng', 'stimer', 'sutils', 'random', 'sprocess', 'sthread',
    'smqueue', 'sproperty', 'ssharedio', 'ssecure', 'emi', 'objectix', 'sbtrace', 'stest', 'sshbuf', 'smap',
    'srandombuff', 'kerncorestats', 'array_eabi', 'byte_pair')] + \
    ['klib/dbase', 'klib/kdes8', 'klib/kheap', 'klib/bma', 'common/alloc', 'common/array', 'common/des8',
     'common/mem', 'common/heap_hybrid', 'common/debugfunction', 'common/secure', 'nkern/nk_old',
     'common/gcchelp']     # the kernel's global operator new/delete (kernel heap); ekern.mmp builds it for MARM

CC32 = ['clang', '--target=armv7a-linux-gnueabihf', '-march=armv7ve', '-marm', '-mfloat-abi=soft',
        '-O2', '-g', '-fno-omit-frame-pointer', '-Wall', '-Wno-unused-parameter', '-ffreestanding', '-fno-stack-protector', '-fno-pic',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fno-exceptions',
        '-I', os.path.join(SRC, 'rt')]


def kernel_flags():
    """C++ flags for kernel code on the hosted platform (__HOSTED__): EKA2 headers, our nk_plat.h first"""
    return ['-x', 'c++', '-std=gnu++11', '-fno-rtti', '-fdeclspec', '-fms-extensions', '-w',
            '-include', os.path.join(EINC, 'gcce/gcce.h'),
            '-include', os.path.join(NKLINUX, 'hosted_prefix.h'),
            '-D__PRODUCT_INCLUDE__=<%s>' % os.path.join(EINC, 'variant/Symbian_OS.hrh'),
            '-D__SYMBIAN32__', '-D__EPOC32__', '-D__EABI__', '-D__GCCE__', '-D__KERNEL_MODE__',
            '-D__HOSTED__', '-D__DEBUGGER_SUPPORT__', '-D__OPERATOR_NEW_DECLARED__', '-D_DEBUG', '-D_UNICODE',
            # as the ARM GCCE kernel build: User::Leave/TRAP are C++ exceptions (__LEAVE_EQUALS_THROW__,
            # Symbian_OS.hrh) only with this; without it ExecHandler::LeaveStart is the invalid-exec
            # handler and the phone's EUSER panics on its first Leave (bug 2026-11-12, kn_efile)
            '-D__SUPPORT_CPP_EXCEPTIONS__',
            '-I', os.path.join(SRC, 'include'), '-I', NKLINUX, '-I', os.path.join(EKA, 'include/nkern'), '-I', os.path.join(EKA, 'include'),
            '-I', EINC, '-I', os.path.join(EINC, 'platform')]


def tool(*names):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    sys.exit('htest: none of %s found' % ', '.join(names))


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('htest: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))


def newer(out, srcs):
    """out is missing or older than a source, or than this script (compile and link flags live here)"""
    srcs = list(srcs) + [os.path.abspath(__file__)]
    return not os.path.exists(out) or any(os.path.getmtime(s) > os.path.getmtime(out) for s in srcs)


def build():
    os.makedirs(os.path.join(OUT, 'obj'), exist_ok=True)
    ld = tool('aarch64-suse-linux-ld', 'aarch64-linux-gnu-ld', 'ld.lld')
    gcc64 = tool('aarch64-suse-linux-gcc', 'aarch64-linux-gnu-gcc')
    objcopy = tool('aarch64-suse-linux-objcopy', 'aarch64-linux-gnu-objcopy', 'llvm-objcopy')
    hdrs = glob.glob(os.path.join(SRC, 'rt', '*.h'))
    rt = []
    for s in sorted(glob.glob(os.path.join(SRC, 'rt', '*.[cS]'))):
        o = os.path.join(OUT, 'obj', os.path.basename(s) + '.o')
        if newer(o, [s] + hdrs):
            run(CC32 + ['-c', s, '-o', o])
        rt.append(o)
    # the nanokernel
    nk = []
    if os.path.isdir(EKA):
        khdrs = hdrs + glob.glob(os.path.join(NKLINUX, '*.h'))
        srcs = [os.path.join(EKA, 'nkern', n + '.cpp') for n in NKERN_GENERIC] + \
            sorted(glob.glob(os.path.join(NKLINUX, '*.cpp')))
        for s in srcs:
            o = os.path.join(OUT, 'nkobj', os.path.basename(s) + '.o')
            os.makedirs(os.path.dirname(o), exist_ok=True)
            if newer(o, [s] + khdrs):
                run(CC32 + kernel_flags() + ['-c', s, '-o', o])
            nk.append(o)
    # the kernel layer: generic sources + our memory model, kernel platform and test variant
    kern = []
    kflags = ['-I', os.path.join(SRC, 'kernel/linux'), '-I', os.path.join(SRC, 'memmodel/linux'),
              '-I', os.path.join(EKA, 'include/kernel'), '-I', os.path.join(EKA, 'common'),
              '-I', os.path.join(EKA, 'include/drivers'), '-D__EXPORT_MEMCPY__']
    if nk:
        khdrs = hdrs + glob.glob(os.path.join(NKLINUX, '*.h')) + glob.glob(os.path.join(SRC, 'kernel/linux/*.h')) + \
            glob.glob(os.path.join(SRC, 'memmodel/linux/*.h')) + glob.glob(os.path.join(SRC, 'include/*.h')) + \
            glob.glob(os.path.join(SRC, 'include/board/*.h')) + [os.path.join(ROOT, 'src/standins/_shared/iscpeer.inl'), os.path.join(ROOT, 'src/qemu-syborg/hw/arm/syborg_modem_core.h')]
        ksrcs = [os.path.join(EKA, n + '.cpp') for n in KERNEL_GENERIC] + \
            sorted(glob.glob(os.path.join(SRC, 'memmodel/linux/*.cpp'))) + \
            sorted(glob.glob(os.path.join(SRC, 'kernel/linux/*.cpp'))) + \
            sorted(glob.glob(os.path.join(SRC, 'variant/*.cpp')))
        # built-in kernel modules (src/hosted/kernel/linux/kmodules.cpp): each defines KernelModuleEntry
        # (and CreateLogicalDevice / CreatePhysicalDevice), so each gets its own names; a generated table
        # maps the ROM file name to them
        kmods = {}
        modules = device_kmodules()
        for mname, romname, msrcs in modules:
            if isinstance(msrcs, str):
                mmp = msrcs if os.path.isabs(msrcs) else os.path.join(EKA, msrcs)
                msrcs = mmp_sources(mmp)
                for m in msrcs:
                    SRC_INCLUDES[m] = MMP_INCLUDES.get(mmp, [])
                    SRC_MACROS[m] = ['-D' + d for d in MMP_MACROS.get(mmp, [])]
            for m in msrcs:
                kmods[m if os.path.isabs(m) else os.path.join(EKA, m)] = mname
        ksrcs += list(kmods)
        table = os.path.join(OUT, 'kmodules_table.cpp')
        lines = ['// generated by tools/hosted/htest.py from KMODULES', '#include <e32def.h>',
                 'struct SHostedModule { const char* iName; TInt (*iEntry)(TInt); TAny* (*iFactory)(); '
                 'void (**iCtors)(); void (**iCtorsEnd)(); TUint8* iData; TUint8* iDataEnd; TUint8* iBss; TUint8* iBssEnd; };']
        for mname, romname, msrcs in modules:
            lines.append('TInt %s_KernelModuleEntry(TInt) __attribute__((weak));' % mname)
            lines.append('TAny* %s_CreateLogicalDevice() __attribute__((weak));' % mname)
            lines.append('TAny* %s_CreatePhysicalDevice() __attribute__((weak));' % mname)
            lines.append('extern "C" void (*__start_kmodinit_%s[])() __attribute__((weak));' % mname)
            lines.append('extern "C" void (*__stop_kmodinit_%s[])() __attribute__((weak));' % mname)
            for sec in ('kmoddata', 'kmodbss'):
                lines.append('extern "C" TUint8 __start_%s_%s[] __attribute__((weak)), __stop_%s_%s[] __attribute__((weak));'
                             % (sec, mname, sec, mname))
        lines.append('extern const SHostedModule HostedModules[] = {')
        for mname, romname, msrcs in modules:
            lines.append('\t{ "%s", %s_KernelModuleEntry, %s_CreateLogicalDevice ? %s_CreateLogicalDevice : %s_CreatePhysicalDevice, '
                         '__start_kmodinit_%s, __stop_kmodinit_%s, __start_kmoddata_%s, __stop_kmoddata_%s, '
                         '__start_kmodbss_%s, __stop_kmodbss_%s },'
                         % (romname, mname, mname, mname, mname, mname, mname, mname, mname, mname, mname))
        lines.append('\t{ 0, 0, 0, 0, 0, 0, 0, 0, 0 } };')
        lines.append('extern const TInt HostedModuleCount = %d;' % len(modules))
        text = '\n'.join(lines) + '\n'
        if not os.path.exists(table) or open(table).read() != text:
            open(table, 'w').write(text)
        ksrcs.append(table)
        modobjs = {}
        for s in ksrcs:
            o = os.path.join(OUT, 'kobj', s.replace(EKA + '/', '').replace(SRC + '/', '').replace('/', '_') + '.o')
            if SRC_MACROS.get(s):      # per-device builds of one source (lcd_n95 vs fb): own objects
                import hashlib
                o = o[:-2] + '-' + hashlib.md5(' '.join(SRC_MACROS[s]).encode()).hexdigest()[:8] + '.o'
            os.makedirs(os.path.dirname(o), exist_ok=True)
            extra = []
            if s in kmods:
                n = kmods[s]
                extra = ['-D%s=%s_%s' % (f, n, f) for f in
                         ('KernelModuleEntry', 'InitExtension', 'CreateLogicalDevice', 'CreatePhysicalDevice')]
                # board/ before everything, the module's own USERINCLUDEs too: its syborg.h routes register
                # access to the hosted I/O bus. Bug 2026-11-12: d_display's .mmp names baseport specific/
                # first, got the real syborg.h (plain memory) and read the framebuffer's refresh rate as 0.
                extra += ['-I', os.path.join(SRC, 'include/board')]
                for d in SRC_INCLUDES.get(s, []):
                    extra += ['-I', d]
                extra += SRC_MACROS.get(s, [])
                extra += ['-I', os.path.dirname(s), '-I', os.path.join(os.path.dirname(s), 'traces'),
                          # our board's headers (syborg.h, boardconfig.h) and the N95's memory model (mmboot.h)
                          '-I', os.path.join(ROOT, 'src/standins/_shared'),
                          # board/ first: its syborg.h routes register access to the hosted I/O bus, then #include_next
                          '-I', os.path.join(SRC, 'include/board'),
                          '-I', os.path.join(ROOT, 'build/epocroot/sf/adapt/qemu/baseport/syborg/specific')]
            if s.endswith('variant/hmodem.cpp'):
                extra = extra + ['-I', MODEM_CORE_DIR, '-I', os.path.join(SRC, 'include/board')]
            if s.endswith('kernel/scodeseg.cpp'):
                # its code segment lookups pass a tagged (odd) pointer as a DCodeSeg& and test bit 0 in the
                # comparator (undefined behaviour); clang -O2 assumes the reference aligned and folds the test
                # away: no code segment is ever found by address or name. Bug 2026-11-12 (kn_boot checks).
                extra = extra + ['-O0']
            if newer(o, [s] + khdrs):
                run(CC32 + kernel_flags() + kflags + extra + ['-c', fixed_source(s), '-o', o])
                if s in kmods:
                    # its static constructors run when the kernel starts the module (as the kernel's
                    # loader runs a kernel DLL's), not before main: own section, run by HostedAddExtension
                    run([objcopy, '--rename-section', '.init_array=kmodinit_%s' % kmods[s], o])
            if s in kmods:
                modobjs.setdefault(kmods[s], []).append(o)
            else:
                kern.append(o)
        # the fake modem's core, shared with the QEMU device (C, built for the hosted runtime)
        core = os.path.join(MODEM_CORE_DIR, 'syborg_modem_core.c')
        o = os.path.join(OUT, 'kobj', 'syborg_modem_core.c.o')
        chdrs = [os.path.join(MODEM_CORE_DIR, 'syborg_modem_core.h'), os.path.join(SRC, 'include/board/syborg_modem_env.h')]
        if newer(o, [core] + chdrs):
            run(CC32 + ['-x', 'c', '-std=gnu11', '-DSYBMODEM_HOSTED', '-I', MODEM_CORE_DIR,
                        '-I', os.path.join(SRC, 'include/board'), '-c', core, '-o', o])
        kern.append(o)
        # each module is a separate DLL on the phone: one partial link per module with only its entry
        # points left global, so modules built from shared sources (iscpeer.inl, nokiastub.h, the NVMemory
        # driver) keep their own classes and data. Weak (inline/template) definitions stay shared.
        # A module's "exports" are the symbols other modules or the kernel use (media drivers import
        # ELOCD's LocDrv/DMediaDriver, as their DLLs do on the phone).
        nmtool = tool('aarch64-suse-linux-nm', 'aarch64-linux-gnu-nm', 'llvm-nm')
        def undefined(objs):
            out = subprocess.run([nmtool, '-u'] + objs, capture_output=True, text=True, check=True).stdout
            return {l.split()[-1] for l in out.splitlines() if l.strip() and not l.endswith(':')}
        for mname, objs in sorted(modobjs.items()):
            mo = os.path.join(OUT, 'kmod', mname + '.o')
            os.makedirs(os.path.dirname(mo), exist_ok=True)
            others = kern + [o for n, os_ in modobjs.items() if n != mname for o in os_]
            if newer(mo, objs) or newer(mo, others) or newer(mo, [os.path.abspath(__file__)]):
                # its writable data in sections of its own: a reloaded module starts with fresh .data/.bss,
                # as a kernel DLL the ARM kernel loads again (kmodules.cpp)
                lds = mo + '.lds'
                open(lds, 'w').write('SECTIONS {\n  kmoddata_%s : { *(.data .data.*) }\n'
                                     '  kmodbss_%s : { *(.bss .bss.* COMMON) }\n}\n' % (mname, mname))
                run([ld, '-m', 'armelf_linux_eabi', '-r', '-T', lds, '-o', mo] + objs)
                keep = {'%s_%s' % (mname, f) for f in ('KernelModuleEntry', 'CreateLogicalDevice', 'CreatePhysicalDevice')}
                keep |= undefined(others)
                nm = subprocess.run([nmtool, mo], capture_output=True, text=True, check=True).stdout
                # weak (inline, template) definitions too: modules built from one source differ in their
                # constants (iscpeer.inl's port), and a shared COMDAT copy would serve both. Bug 2026-11-12:
                # NokiaISCDriverExtension ran NokiaISCDriver's inline Install() (port 0's interrupt, in use).
                # The COMDAT groups go first, so the final link does not fold the module's copies away.
                loc = [l.split()[2] for l in nm.splitlines()
                       if len(l.split()) == 3 and l.split()[1] in 'TDBRWV' and l.split()[2] not in keep]
                lf = mo + '.localize'
                open(lf, 'w').write('\n'.join(loc) + '\n')
                run([objcopy, '--remove-section=.group', '--localize-symbols=' + lf, mo])
        kern += [os.path.join(OUT, 'kmod', n + '.o') for n in sorted(modobjs)]
    tests = []
    for s in sorted(glob.glob(os.path.join(SRC, 'tests', 'kn_*.cpp'))):
        if not kern:
            continue
        name = os.path.splitext(os.path.basename(s))[0]
        o = os.path.join(OUT, 'obj', name + '.o')
        exe = os.path.join(OUT, 't', name)
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        if newer(o, [s] + hdrs + glob.glob(os.path.join(SRC, 'tests', '*.h'))):
            run(CC32 + kernel_flags() + kflags + ['-I', os.path.join(SRC, 'tests'), '-c', s, '-o', o])
        if newer(exe, [o] + rt + nk + kern):
            # the hosted kernel's own image lives outside every Symbian region (user data 0x00400000+,
            # ROM 0x80000000+, kernel data 0xC8000000-0xC9200000): at 0xD0000000. Bug 2026-11-12: at
            # 0x10000 its .bss covered 0x00400000 and efile.exe's data area could not be mapped.
            run([ld, '-m', 'armelf_linux_eabi', '-static', '-e', '_start', '-Ttext-segment=0xd0000000',
                 '-o', exe, o] + kern + nk + rt)
        tests.append(exe)
    # Symbian's standalone nanokernel suite (kerneltest/e32test/nkernsa) on the hosted platform: nk_sa
    sa = os.path.join(ROOT, 'build/epocroot/sf/os/kernelhwsrv/kerneltest/e32test/nkernsa')
    if nk and os.path.isdir(sa):
        inc = os.path.join(OUT, 'nkinc')
        os.makedirs(inc, exist_ok=True)
        if not os.path.islink(os.path.join(inc, 'nktest')):
            os.symlink(sa, os.path.join(inc, 'nktest'))
        skip = {'nirqx.cpp', 'tlsf.cpp'}          # SMP interrupt controller; TLSF heap from a ROM super page
        srcs = [os.path.join(sa, f) for f in sorted(os.listdir(sa)) if f.endswith('.cpp') and f not in skip]
        srcs.append(os.path.join(SRC, 'nkern', 'nksa', 'nksa_hosted.cpp'))
        objs = []
        for s in srcs:
            o = os.path.join(OUT, 'saobj', os.path.basename(s) + '.o')
            os.makedirs(os.path.dirname(o), exist_ok=True)
            if newer(o, [s] + hdrs + glob.glob(os.path.join(NKLINUX, '*.h'))):
                run(CC32 + kernel_flags() + ['-I', inc, '-I', os.path.join(EKA, 'include/kernel'),
                                             '-D__STANDALONE_NANOKERNEL__', '-c', s, '-o', o])
            objs.append(o)
        exe = os.path.join(OUT, 't', 'nk_sa')
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        if newer(exe, objs + nk + rt):
            run([ld, '-m', 'armelf_linux_eabi', '-static', '-e', '_start', '-Ttext-segment=0x10000',
                 '-o', exe] + objs + nk + rt)
        tests.append(exe)
    for s in sorted(glob.glob(os.path.join(SRC, 'tests', 'nk_*.cpp'))):
        if not nk:
            print('htest: no EKA2 tree (scripts/setup-epocroot.sh): skipping %s' % os.path.basename(s))
            continue
        name = os.path.splitext(os.path.basename(s))[0]
        o = os.path.join(OUT, 'obj', name + '.o')
        exe = os.path.join(OUT, 't', name)
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        if newer(o, [s] + hdrs + glob.glob(os.path.join(NKLINUX, '*.h'))):
            run(CC32 + kernel_flags() + ['-c', s, '-o', o])
        if newer(exe, [o] + rt + nk):
            run([ld, '-m', 'armelf_linux_eabi', '-static', '-e', '_start', '-Ttext-segment=0x10000',
                 '-o', exe, o] + nk + rt)
        tests.append(exe)
    plain = sorted(glob.glob(os.path.join(SRC, 'tests', 't_*.c')) + glob.glob(os.path.join(SRC, 'tests', 't_*.cpp')))
    for s in plain:
        name = os.path.splitext(os.path.basename(s))[0]
        o = os.path.join(OUT, 'obj', name + '.o')
        exe = os.path.join(OUT, 't', name)
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        if newer(o, [s] + hdrs):
            cxx = ['-x', 'c++', '-fno-rtti'] if s.endswith('.cpp') else []
            run(CC32 + cxx + ['-c', s, '-o', o])
        if newer(exe, [o] + rt):
            lflag = ['-m', 'armelf_linux_eabi'] if 'lld' not in ld else []
            run([ld] + lflag + ['-static', '-e', '_start', '-Ttext-segment=0x10000', '--gc-sections',
                                '-o', exe, o] + rt)
        tests.append(exe)
    init = os.path.join(OUT, 'init')
    isrc = os.path.join(SRC, 'init', 'init.c')
    if newer(init, [isrc]):
        run([gcc64, '-static', '-O2', '-Wall', '-o', init, isrc])
    return init, tests


def cpio(path, entries):
    """newc cpio: entries = [(name, mode, data or None for a directory)]"""
    out = bytearray()

    def add(name, mode, data, ino):
        name_b = name.encode() + b'\0'
        hdr = '070701' + ''.join('%08x' % v for v in (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name_b), 0))
        out.extend(hdr.encode() + name_b)
        out.extend(b'\0' * (-len(out) % 4))
        out.extend(data)
        out.extend(b'\0' * (-len(out) % 4))
    for i, (name, mode, data) in enumerate(entries):
        add(name, mode, data or b'', i + 1)
    add('TRAILER!!!', 0, b'', 0)
    with open(path, 'wb') as f:
        f.write(out)


SEED_SHARE = None


def rom_setup(dev):
    """the phone ROM for the ROM tests (nk_rom): (share dir, args text) or (None, None).
    Only addresses go into the initramfs; the ROM itself is shared read-only over 9p from its build dir."""
    if not dev:
        return None, None
    env = dict(os.environ, SYBORG_DEVICE=dev)
    try:
        out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'paths'], env=env,
                             capture_output=True, text=True, check=True).stdout
    except subprocess.CalledProcessError:
        return None, None
    paths = dict(l.split(None, 1) for l in out.splitlines() if ' ' in l)
    # the composed (hybrid) ROM, as the ARM machine boots it: the phone's user side at its own addresses,
    # our kernel-side modules (the hosted kernel substitutes its own builds of them by name) and our
    # header (personality bits). EUSER is the phone's: its exports/stubs come from the stock index.
    flat, cj = paths.get('rom', '').strip(), paths.get('core_json', '').strip()
    if not (os.path.exists(flat) and os.path.exists(cj)):
        return None, None
    real = os.path.realpath(flat)
    idx = json.load(open(cj))
    eu = [f for f in idx['files'] if f['path'].lower().endswith('/euser.dll')]
    if not eu:
        return None, None
    sys.path.insert(0, os.path.join(ROOT, 'tools/hosted'))
    import romscan
    img = open(real, 'rb').read()
    im = eu[0]['image']
    stubs = romscan.scan(img, idx['base'], im['code'], im['textsize'] & ~3)['stubs']
    if not stubs:
        return None, None      # EUSER not where the stock index says: not a hybrid of this firmware
    lo, hi = min(a for a, _ in stubs), max(a for a, _ in stubs) + 8
    # the exec personality the device's user side needs (registry id; the kernel picks its tables by it)
    pers = 0
    try:
        name = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'get', 'platform.personality'],
                              env=env, capture_output=True, text=True, check=True).stdout.strip()
        reg = open(os.path.join(ROOT, 'personalities/registry.toml')).read()
        m = re.search(r'^%s\s*=\s*(\d+)' % re.escape(name), reg, re.M)
        pers = int(m.group(1)) if m else 0
    except (subprocess.CalledProcessError, OSError):
        pass
    args = 'rom /rom/%s\nbase %x\nsize %x\neuser_expdir %x\nstubs %x %x\npersonality %x\n' % (
        os.path.basename(real), idx['base'], len(img), im['expdir'], lo, hi, pers)
    try:
        qa = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'qemu-args'], env=env,
                            capture_output=True, text=True, check=True).stdout.split()
        for x in qa:
            if x.startswith('syborg-platform.config='):
                args += 'boardconfig %s\n' % x.split('=', 1)[1]
            if x.startswith('syborg-fb.width=') or x.startswith('syborg-fb.height='):
                args += 'fb%s %s\n' % (x[10], x.split('=', 1)[1])      # fbw / fbh
            if x.startswith('syborg-modem.replies='):
                # the fake modem's reply table (generated next to the ROM: inside the /rom share)
                rel = os.path.relpath(os.path.realpath(x.split('=', 1)[1]), os.path.dirname(real))
                if not rel.startswith('..'):
                    args += 'replies /rom/%s\n' % rel
    except subprocess.CalledProcessError:
        pass
    if os.path.exists(real + '.rofs'):
        args += 'rofs /rom/%s.rofs\n' % os.path.basename(real)      # the flash window (medrofs)
    # the drives past first boot (C: and E: NVMemory images), shared read-only; tests map them private
    global SEED_SHARE
    seed = os.path.join(ROOT, 'build/seeds', dev)
    if os.path.exists(os.path.join(seed, 'nvmemory.img')):
        SEED_SHARE = seed
        args += 'nvmem /seed/nvmemory.img\n'
        if os.path.exists(os.path.join(seed, 'nvmemory2.img')):
            args += 'nvmem2 /seed/nvmemory2.img\n'
    return os.path.dirname(real), args


def pack(init, tests, extra=()):
    d = stat.S_IFDIR | 0o755
    x = stat.S_IFREG | 0o755
    ents = [('dev', d, None), ('proc', d, None), ('sys', d, None), ('t', d, None),
            ('init', x, open(init, 'rb').read())]
    ents += [('t/' + os.path.basename(t), x, open(t, 'rb').read()) for t in tests]
    ents += [(n, stat.S_IFREG | 0o644, data.encode()) for n, data in extra]
    img = os.path.join(OUT, 'initramfs.cpio')
    cpio(img, ents)
    return img


# tests that need more than the default per-test time (seconds, under TCG); they run only when named
SLOW = {'nk_sa': 900}

MATRIX = [('cortex-a55', 1), ('cortex-a55', 2), ('cortex-a55', 4), ('max', 4)]


def boot_one(img, tests, cpu, smp, a, tag):
    """one QEMU run of `tests` (all when empty); returns (results {name: (res, code, ms)}, summary, log, lines)"""
    append = 'console=ttyAMA0 panic=-1 quiet'
    if tests:
        append += ' htest=' + ','.join(tests)
    append += ' htest.timeout=%d htest.cp15=%s %s' % (a.timeout, a.cp15, a.append)
    for kv in a.env + (['KTRACE=%s' % a.trace] if a.trace else []) + (['XTRACE=%d' % a.xtrace] if a.xtrace else []):
        append += ' htest.env.' + kv
    for n in (tests or SLOW):
        if n in SLOW and not a.timeout_given:
            append += ' htest.t.%s=%d' % (n, SLOW[n])
    cmd = [QEMU, '-M', 'virt,gic-version=3', '-accel', 'tcg', '-cpu', cpu, '-smp', str(smp), '-m', '1024',
           '-kernel', KERNEL, '-initrd', img, '-append', append.strip(),
           '-nographic', '-no-reboot', '-serial', 'stdio', '-monitor', 'none']
    if a.rom_share:
        cmd += ['-virtfs', 'local,path=%s,mount_tag=rom,security_model=none,readonly=on' % a.rom_share]
    if SEED_SHARE:
        cmd += ['-virtfs', 'local,path=%s,mount_tag=seed,security_model=none,readonly=on' % SEED_SHARE]
    # what tests produce for the host (framebuffer dumps): /out in the VM, one directory per run
    vmout = os.path.join(OUT, 'vmout', tag or 'run')
    os.makedirs(vmout, exist_ok=True)
    cmd += ['-virtfs', 'local,path=%s,mount_tag=out,security_model=mapped-xattr' % vmout]
    log = os.path.join(OUT, 'console%s.log' % ('-' + tag if tag else ''))
    total_limit = 60 + a.timeout * (len(tests) or 30) + (0 if a.timeout_given else sum(SLOW.get(n, 0) for n in (tests or SLOW)))
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    results, summary, lines = {}, None, []
    with open(log, 'w') as lf:
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
            lf.write(line + '\n')
            lines.append(line)
            m = re.match(r'HTEST-RESULT (\S+) (\S+) (-?\d+) (\d+)', line)
            if m:
                results[m.group(1)] = (m.group(2), int(m.group(3)), int(m.group(4)))
            if line.startswith('HTEST-SUMMARY'):
                summary = line
            if a.live and (a.verbose or line.startswith('HTEST') or line.startswith('  ')):
                print(line, flush=True)
            if time.time() - t0 > total_limit:
                p.kill()
                lines.append('htest: overall time limit reached')
                break
    p.wait()
    return results, summary, log, lines


def test_output(lines, name):
    """the lines a test printed (between its BEGIN and RESULT)"""
    out, on = [], False
    for l in lines:
        if l == 'HTEST-BEGIN ' + name:
            on = True
            continue
        if l.startswith('HTEST-RESULT ' + name + ' '):
            break
        if on:
            out.append(l)
    return out


PHONE_SHM_ADDR = 5        # the ivshmem device's PCI slot: /sys/bus/pci/devices/0000:00:05.0/resource2 in the VM


def run_phone(img, a):
    """the phone (kn_efile in PHONE mode) with its screen in host shared memory: an ivshmem device whose memory
    is the file a.shm (tools/hosted/phone.py draws it and writes keys into it); serial log in OUT/phone.log;
    returns when the VM ends"""
    shm = a.shm or os.path.join(OUT, 'phone.shm')
    serial = a.serial or os.path.join(OUT, 'phone.log')
    with open(shm, 'wb') as f:
        f.truncate(1 << 20)
    append = ('console=ttyAMA0 panic=-1 quiet htest=kn_efile htest.timeout=100000000 htest.cp15=%s '
              'htest.env.PHONE=/sys/bus/pci/devices/0000:00:%02x.0/resource2 %s' % (a.cp15, PHONE_SHM_ADDR, a.append))
    for kv in a.env:
        append += ' htest.env.' + kv
    cmd = [QEMU, '-M', 'virt,gic-version=3', '-accel', 'tcg', '-cpu', a.cpu, '-smp', str(a.smp), '-m', '1024',
           '-kernel', KERNEL, '-initrd', img, '-append', append.strip(), '-no-reboot', '-display', 'none',
           '-object', 'memory-backend-file,id=phonefb,size=1M,mem-path=%s,share=on' % shm,
           '-device', 'ivshmem-plain,memdev=phonefb,addr=0x%x' % PHONE_SHM_ADDR,
           '-name', 'hosted %s' % a.rom, '-serial', 'file:%s' % serial, '-monitor', 'none']
    if a.qmp:
        cmd += ['-qmp', 'unix:%s,server,nowait' % a.qmp]
    if a.rom_share:
        cmd += ['-virtfs', 'local,path=%s,mount_tag=rom,security_model=none,readonly=on' % a.rom_share]
    if SEED_SHARE:
        cmd += ['-virtfs', 'local,path=%s,mount_tag=seed,security_model=none,readonly=on' % SEED_SHARE]
    print('htest: phone %s, screen in %s, serial log %s' % (a.rom, os.path.relpath(shm, ROOT),
                                                           os.path.relpath(serial, ROOT)),
          flush=True)
    import signal
    q = subprocess.Popen(cmd)
    def stop(*_):
        q.terminate()       # never leave the VM behind (an interrupted or timed-out htest)
        sys.exit(130)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    return q.wait()


def run_all(img, names, a):
    for p, how in ((QEMU, 'scripts/build-hosted-qemu.sh'), (KERNEL, 'scripts/build-hosted-kernel.sh')):
        if not os.path.exists(p):
            sys.exit('htest: %s missing: run %s' % (p, how))
    configs = MATRIX if a.matrix else [(a.cpu, a.smp)]
    jobs = max(1, a.jobs)
    shards = [names[i::jobs] for i in range(jobs)] if jobs > 1 else [list(a.tests)]
    shards = [sh for sh in shards if sh or jobs == 1]
    runs = [(cpu, smp, i, sh) for cpu, smp in configs for i, sh in enumerate(shards)]
    a.live = len(runs) == 1
    t0 = time.time()
    from concurrent.futures import ThreadPoolExecutor
    width = min(len(runs), max(1, (os.cpu_count() or 4) // 2))
    with ThreadPoolExecutor(max_workers=width) as ex:
        # a log per run, named after the config and its tests, so that concurrent htest invocations
        # (e.g. a long nk_sa run beside quick runs) do not write the same file
        def tag(r):
            names_part = '-'.join(r[3])[:60] if r[3] else 'all'
            return '%s-smp%d-%s' % (r[0], r[1], names_part) if len(runs) == 1 else '%s-smp%d-%d' % (r[0], r[1], r[2])
        futs = [(r, ex.submit(boot_one, img, r[3], r[0], r[1], a, tag(r))) for r in runs]
        done = [(r, f.result()) for r, f in futs]
    ok = True
    table = {}
    for (cpu, smp, i, sh), (results, summary, log, lines) in done:
        cfg = '%s/smp%d' % (cpu, smp)
        expect = sh or names
        for n in expect:
            res = results.get(n, ('MISSING', -1, 0))
            table.setdefault(n, {})[cfg] = res
            if res[0] not in ('PASS', 'SKIP'):
                ok = False
                if not a.live:
                    print('--- %s on %s: %s (%s)' % (n, cfg, res[0], os.path.relpath(log, ROOT)))
                    for l in test_output(lines, n)[-25:]:
                        if not l.startswith('  ok '):
                            print(l)
        if summary is None:
            ok = False
            print('htest: %s shard %d: no summary (guest died?), %s' % (cfg, i, os.path.relpath(log, ROOT)))
    if not a.live:
        cfgs = ['%s/smp%d' % c for c in configs]
        print('%-16s' % 'test' + ''.join('%-18s' % c for c in cfgs))
        for n in sorted(table):
            print('%-16s' % n + ''.join('%-18s' % ('%s %dms' % table[n][c][0::2] if c in table[n] else '-') for c in cfgs))
    npass = sum(1 for n in table for c in table[n] if table[n][c][0] in ('PASS', 'SKIP'))
    nall = sum(len(table[n]) for n in table)
    print('htest: %d/%d passed, %d QEMU runs (%d in parallel), %.1f s' % (npass, nall, len(runs), width, time.time() - t0))
    return 0 if ok and nall else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('tests', nargs='*')
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--phone', action='store_true',
                    help='boot the device to its home screen in a window (virtio-gpu, keyboard); runs until closed')
    ap.add_argument('--serial', help='--phone: the serial log file (default build/hosted/htest/phone.log; '
                    'a second phone at once needs its own --shm and --serial)')
    ap.add_argument('--shm', help='--phone: the shared screen/keys file (default build/hosted/htest/phone.shm)')
    ap.add_argument('--qmp', help='--phone: a QMP unix socket path')
    ap.add_argument('--cpu', default='cortex-a55')
    ap.add_argument('--smp', type=int, default=4)
    ap.add_argument('--timeout', type=int, default=None, help='per-test limit in seconds (default 60; SLOW tests more)')
    ap.add_argument('--cp15', default='1', choices='012')
    ap.add_argument('--append', default='')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='environment for the tests')
    ap.add_argument('--trace', metavar='MASK', help='kernel trace mask for kn_* tests (hex, nk_trace.h bits: '
                    '2 KBOOT, 0x40 KPROC, 0x80 KEXEC, 0x200 KTHREAD, 0x400 KDLL)')
    ap.add_argument('--xtrace', type=int, default=0, metavar='N', help='print the first N trapped exec calls '
                    '(name them: tools/hosted/execnames.py)')
    ap.add_argument('-j', '--jobs', type=int, default=1, help='split the tests over N QEMUs run in parallel')
    ap.add_argument('--rom', default=os.environ.get('SYBORG_DEVICE', 'nokia-n95-8gb-rm320'),
                    help='device whose composed core ROM the ROM tests map (shared read-only); "" for none')
    ap.add_argument('--matrix', action='store_true',
                    help='run on every machine config (%s) in parallel' % ', '.join('%s/smp%d' % c for c in MATRIX))
    a = ap.parse_args()
    a.timeout_given = a.timeout is not None
    if a.timeout is None:
        a.timeout = 60
    global DEVICE
    DEVICE = a.rom
    init, tests = build()
    if a.tests:
        known = {os.path.basename(t) for t in tests}
        bad = [t for t in a.tests if t not in known]
        if bad:
            sys.exit('htest: unknown tests %s (have %s)' % (bad, sorted(known)))
    a.rom_share, rom_args = rom_setup(a.rom)
    img = pack(init, tests, [('t/nk_rom.args', rom_args)] if rom_args else [])   # read by nk_rom, kn_*
    if a.phone:
        return run_phone(img, a)
    if a.build_only:
        print('htest: built %d tests, %s' % (len(tests), os.path.relpath(img, ROOT)))
        return 0
    names = list(a.tests) or sorted(os.path.basename(t) for t in tests if os.path.basename(t) not in SLOW)
    return run_all(img, names, a)


if __name__ == '__main__':
    sys.exit(main())
