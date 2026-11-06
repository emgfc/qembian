#!/usr/bin/env python3
"""syb-gdb: runtime probes on the N95 hybrid under modern QEMU (boots it, attaches gdb, cleans up).

  syb_gdb.py leaves [--modules M1,M2] [--err E] [--secs N] [--max K]
        break on EUser User::Leave; print error, current thread and the symbolised call chain
        (code addresses found on the user stack); keep only chains through the given modules
  syb_gdb.py brk LOC [--print EXPR]... [--stack] [--secs N] [--max K] [--when-thread SUBSTR] [--after S]
        hardware breakpoint at LOC (hex address, EUser-style export name like 'User::Leave', or a
        kernel symbol); print registers, the current thread, optional gdb expressions and stack
  syb_gdb.py sample [--at T] [--n N] [--interval S]
        PC samples after T seconds: mode, current thread, PC/LR and stack chain, symbolised
  syb_gdb.py thread [--at T]       name of the thread running at T seconds

Options common to all: --no-compose (default: reuse build/n95/hybrid.img as is), --keep (leave
QEMU running), --keep-drives DIR (boot with DIR/nvmemory.img, nvmemory2.img and rtc-offset, as
written by syb_run.py --keep-drives; default: fresh drives, RTC = host time). The kernel's DWARF (_syborg_ekern.exe.sym) is loaded at its ROM/data addresses,
so kernel expressions work in --print (e.g. TheScheduler.iCurrentThread->iPriority).
Output goes to stdout and build/runs/gdb-<cmd>.log.
"""
import argparse, json, os, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel  # tools/device.py: build paths of the device ($SYBORG_DEVICE / devices/default)
DEVPATHS = devmodel.paths(devmodel.load())
DRIVES = None
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ksyms
REL = os.path.join(ROOT, 'build', 'epocroot', 'epoc32', 'release', 'armv5', 'udeb')
KSYM = os.path.join(REL, '_syborg_ekern.exe.sym')

GDB_PY = r'''
import gdb, sys, re, json, time
T0 = time.time()
sys.path.insert(0, %(tools)r)
import syb_sym
R = syb_sym.Rom()
CFG = json.loads(%(cfg)r)
OUT = open(CFG['log'], 'a')

def emit(s):
    print(s); OUT.write(s + '\n'); OUT.flush()

def hbuf(p):
    p = int(p)
    if not p:
        return '?'
    try:
        n = int(gdb.parse_and_eval('*(unsigned int*)%%d' %% p)) & 0x0fffffff
        return bytes(gdb.selected_inferior().read_memory(p + 8, min(n, 80))).decode('latin-1')
    except Exception:
        return '?'

def thread_name():
    try:
        nt = int(gdb.parse_and_eval('(unsigned int)TheScheduler.iCurrentThread'))
        off = int(gdb.parse_and_eval('(unsigned int)&((DThread*)0)->iNThread'))
        dt = gdb.parse_and_eval('(DThread*)%%d' %% (nt - off))
        return '%%s::%%s' %% (hbuf(dt['iOwningProcess']['iName']), hbuf(dt['iName']))
    except Exception as e:
        return '?(%%s)' %% e

def reg(r):
    return int(gdb.parse_and_eval('$' + r)) & 0xffffffff

def stack_words(n=96):
    """up to n words from sp, stopping at the end of mapped memory (a supervisor stack is often
    only a few words deep at an exec call: reading 96 words in one go used to fail silently)"""
    sp, mem = reg('sp'), b''
    step = 64
    while len(mem) < 4 * n:
        try:
            mem += bytes(gdb.selected_inferior().read_memory(sp + len(mem), step))
        except Exception:
            if step == 4:
                break
            step = 4
    return [int.from_bytes(mem[i:i+4], 'little') for i in range(0, len(mem) - 3, 4)]

EXEC92 = {}
try:
    for _l in open(%(tools)r + '/exec92/exec92.map'):
        _f = _l.split()
        if len(_f) >= 2 and _f[0][:1] in 'FS' and len(_f[0]) == 3:
            EXEC92[_f[0]] = _f[1]
except OSError:
    pass

def user_site():
    """at a kernel-mode stop inside an exec call: the user return PC is the last word of the
    supervisor stack; the exec number is in the SWI just before it"""
    if reg('cpsr') & 0x1f == 0x10:
        return None
    ws = stack_words(512)
    if not ws:
        return None
    pc = ws[-1]
    m = R.module_at(pc & ~1)
    if not m:
        return None
    s = 'user pc ' + R.describe(pc, short=True)
    try:
        if not pc & 1:
            insn = int.from_bytes(bytes(gdb.selected_inferior().read_memory(pc - 4, 4)), 'little')
            if insn >> 24 == 0xef:
                n = insn & 0xffffff
                key = '%%s%%02x' %% ('F' if n & 0x800000 else 'S', n & 0xff)
                s += ' (9.2 exec %%s %%s)' %% (key, EXEC92.get(key, '?'))
    except Exception:
        pass
    return s

def chain(n=96, keep=8):
    ws = stack_words(n)
    out = []
    for k, w in enumerate(ws):
        m = R.module_at(w & ~1)
        if m and (w & ~1) < m['text']:
            d = R.describe(w, short=True)
            if not out or out[-1] != d:
                out.append(d)
        if len(out) >= keep:
            break
    return out

class Probe(gdb.Breakpoint):
    def __init__(self, loc, kind):
        super().__init__(loc, type=gdb.BP_HARDWARE_BREAKPOINT, internal=False)
        self.kind = kind
        self.hits = 0
        try:
            a = int(gdb.parse_and_eval('(unsigned int)&%%s' %% loc.lstrip('*')) if not loc.startswith('*0x')
                    else int(loc[1:], 16))
            emit('PROBE %%s at %%08x %%s' %% (loc, a, R.describe(a, short=True)))
        except Exception as e:
            emit('PROBE %%s (address not resolved here: %%s)' %% (loc, e))
    def stop(self):
        if CFG.get('after') and time.time() - T0 < CFG['after']:
            return False
        th = thread_name()
        if CFG.get('when_thread') and CFG['when_thread'] not in th:
            return False
        if self.hits >= CFG['max']:
            return False
        if self.kind == 'leave':
            err = reg('r0') - (1 << 32) if reg('r0') & 0x80000000 else reg('r0')
            if CFG.get('err') is not None and err != CFG['err']:
                return False
            ch = chain()
            mods = CFG.get('modules')
            if mods and not any(any(m.lower() in c.lower() for m in mods) for c in ch):
                return False
            self.hits += 1
            emit('LEAVE %%d  %%s\n    ' %% (err, th) + '\n    '.join(ch))
        else:
            self.hits += 1
            emit('BRK %%08x  %%s  r0=%%08x r1=%%08x r2=%%08x r3=%%08x lr=%%s' %% (
                reg('pc'), th, reg('r0'), reg('r1'), reg('r2'), reg('r3'), R.describe(reg('lr'), short=True)))
            us = user_site()
            if us:
                emit('    ' + us)
            for e in CFG.get('print', []):
                try:
                    emit('    %%s = %%s' %% (e, gdb.parse_and_eval(e)))
                except Exception as ex:
                    emit('    %%s: %%s' %% (e, ex))
            if CFG.get('stack'):
                emit('    ' + '\n    '.join(chain()))
        return False

def sample():
    pc, lr, cpsr = reg('pc'), reg('lr'), reg('cpsr')
    mode = {0x10: 'usr', 0x13: 'svc', 0x12: 'irq', 0x11: 'fiq', 0x17: 'abt', 0x1b: 'und', 0x1f: 'sys'}.get(cpsr & 0x1f, '?')
    emit('SAMPLE %%s %%-40s pc %%s | lr %%s' %% (mode, thread_name(), R.describe(pc, short=True), R.describe(lr, short=True)))
    if mode == 'usr':
        emit('    ' + ' <- '.join(chain(keep=5)))
'''


def gdb_cmds(cfg, extra):
    return ['gdb', '-nx', '-batch',
            '-ex', 'set pagination off', '-ex', 'set confirm off', '-ex', 'set architecture arm',
            '-ex', ksyms.add_symbol_file(KSYM),
            '-ex', 'target remote 127.0.0.1:1234'] + extra


QMP_SOCK = os.path.join(ROOT, 'build', 'runs', 'gdb-qmp.sock')


def send_keys(keys):
    """--key T:K: send QMP key chords at T seconds of wall clock after QEMU start (the guest runs
    slower under gdb, so allow some margin)"""
    import threading
    def run():
        t0 = time.time()
        for spec in sorted(keys, key=lambda k: float(k.split(':', 1)[0])):
            t, k = spec.split(':', 1)
            time.sleep(max(0, float(t) - (time.time() - t0)))
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'qmp.py'), QMP_SOCK, 'key', k],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if keys:
        threading.Thread(target=run, daemon=True).start()


def start_qemu(wait, secs):
    subprocess.run(['pkill', '-f', 'qemu-system-arm.*-name sybtool'])
    time.sleep(0.5)
    cmd = [os.path.join(ROOT, 'scripts', 'run-modern.sh'), '--timeout', str(int(secs) + 5), '--name', 'sybtool',
           '--gdb-wait' if wait else '--gdb', '--dtb', DEVPATHS['dtb'],
           '--qmp', QMP_SOCK,
           *(['--nvmem', os.path.join(DRIVES, 'nvmemory.img'), '--nvmem2', os.path.join(DRIVES, 'nvmemory2.img')]
             if DRIVES else []),
           '--serial-log', os.path.join(ROOT, 'build', 'runs', 'gdb-serial.log'),
           os.path.abspath(ROM_PATH)]
    if DRIVES:
        cmd += ['-global', 'syborg-rtc.offset-file=' + os.path.join(DRIVES, 'rtc-offset')]
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(2)
    return p


def write_py(cfg):
    fd, path = tempfile.mkstemp(suffix='.py', prefix='sybgdb-')
    os.write(fd, (GDB_PY % {'tools': os.path.join(ROOT, 'tools'), 'cfg': json.dumps(cfg)}).encode())
    os.close(fd)
    return path


def resolve(loc):
    try:
        return '*0x%x' % (int(loc, 16) & ~1)
    except ValueError:
        pass
    import syb_sym
    r = syb_sym.Rom()
    for e in r.ex:
        if loc == e[4] or loc == e[3] or e[4].startswith(loc + '('):
            return '*0x%x' % e[0]
    return loc       # let gdb resolve it (kernel symbol)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['leaves', 'brk', 'sample', 'thread'])
    ap.add_argument('loc', nargs='?')
    ap.add_argument('--modules')
    ap.add_argument('--err', type=int)
    ap.add_argument('--secs', type=float, default=30)
    ap.add_argument('--max', type=int, default=50)
    ap.add_argument('--print', action='append', default=[])
    ap.add_argument('--stack', action='store_true')
    ap.add_argument('--when-thread')
    ap.add_argument('--after', type=float, default=0, help='ignore hits in the first N seconds (wall clock, ~ guest time)')
    ap.add_argument('--at', type=float, default=20)
    ap.add_argument('--n', type=int, default=10)
    ap.add_argument('--interval', type=float, default=1)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--keep-drives', metavar='DIR')
    ap.add_argument('--key', action='append', default=[], help='T:KEYS, QMP key chord at T s (wall clock)')
    ap.add_argument('--rom', default=DEVPATHS['rom'])
    a = ap.parse_args()
    global ROM_PATH, DRIVES
    ROM_PATH = a.rom
    DRIVES = os.path.abspath(a.keep_drives) if a.keep_drives else None
    os.makedirs(os.path.join(ROOT, 'build', 'runs'), exist_ok=True)
    log = os.path.join(ROOT, 'build', 'runs', 'gdb-%s.log' % a.cmd)
    gdbout = os.path.join(ROOT, 'build', 'runs', 'gdb-%s.gdb.txt' % a.cmd)
    open(log, 'w').close()
    cfg = {'log': log, 'max': a.max, 'err': a.err, 'print': a.print, 'stack': a.stack,
           'when_thread': a.when_thread, 'after': a.after, 'modules': a.modules.split(',') if a.modules else None}
    py = write_py(cfg)
    try:
        if a.cmd in ('leaves', 'brk'):
            loc = resolve('User::Leave(int)' if a.cmd == 'leaves' else a.loc)
            kind = 'leave' if a.cmd == 'leaves' else 'brk'
            q = start_qemu(True, a.secs)
            send_keys(a.key)
            with open(gdbout, 'w') as go:
                subprocess.run(gdb_cmds(cfg, ['-ex', 'source ' + py, '-ex', 'python Probe(%r, %r)' % (loc, kind),
                                              '-ex', 'continue']), timeout=a.secs + 20,
                               stdout=go, stderr=subprocess.STDOUT)
        else:
            q = start_qemu(False, max(a.secs, a.at + a.n * (a.interval + 1) + 5))
            time.sleep(max(0, a.at - 2))
            for _ in range(a.n if a.cmd == 'sample' else 1):
                subprocess.run(gdb_cmds(cfg, ['-ex', 'source ' + py, '-ex', 'python sample()' if a.cmd == 'sample'
                                              else 'python emit("THREAD " + thread_name())', '-ex', 'detach']),
                               timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(a.interval)
    except subprocess.TimeoutExpired:
        pass
    finally:
        os.unlink(py)
        if not a.keep:
            subprocess.run(['pkill', '-f', 'qemu-system-arm.*-name sybtool'])
    text = open(log).read()
    print(text, end='')
    if a.cmd in ('leaves', 'brk') and 'BRK' not in text and 'LEAVE' not in text:
        print('no hits in %ss (gdb output: %s):' % (a.secs, os.path.relpath(gdbout, ROOT)))
        try:
            print(''.join([l for l in open(gdbout).readlines() if l.strip()][-8:]), end='')
        except OSError:
            pass


if __name__ == '__main__':
    main()
