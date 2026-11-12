#!/usr/bin/env python3
"""A persistent N-Gage (EKA1) debug session: boot once, freeze on a crash, inspect many times.

The session's daemon owns QEMU and a gdb-protocol connection to it. Catches are hardware
breakpoints; when one is hit, the machine stays frozen, the daemon writes a crash dump (all of
SDRAM + registers + the kernel's fault words) and every later command inspects that frozen
machine, without another boot.

  ngage_sess.py [-s NAME] start [--catch fatal,pabt,dabt,VA=LABEL] [--debug unimp,guest_errors]
  ngage_sess.py [-s NAME] wait [SECS]        until a catch freezes the machine (prints the summary)
  ngage_sess.py [-s NAME] status | regs | x VA [N] | objects | stack THREAD | shot FILE.png
  ngage_sess.py [-s NAME] break VA[=LABEL] | del VA | cont | pause | mon 'MONITOR COMMAND'
  ngage_sess.py [-s NAME] dump [DIR]         SDRAM + registers to files (done by itself on a catch)
  ngage_sess.py [-s NAME] stop

Catch names: fatal = the kernel fatal-fault routine (0x5031e340), pabt / dabt / undef = the
exception vectors (low vectors, VA 0x0c / 0x10 / 0x04). Any VA also works. Session files are in
build/ngage/sess-NAME/ (qemu.log, serial, crash-N/). Offline: `ngage_sess.py dumpx DIR VA [N]`
reads a crash dump's RAM through its page tables, with no machine running.
"""
import argparse, json, os, pathlib, select, signal, socket, struct, subprocess, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ngage_run as nr  # noqa: E402

ROOT = nr.ROOT
SDRAM, SDRAM_SIZE = 0x0A000000, 0x01000000
CATCHES = {'fatal': 0x5031e340, 'pabt': 0x0c, 'dabt': 0x10, 'undef': 0x04}
CUR_THREAD = 0x80006740        # the kernel's current DThread
FAULT_WORDS = 0x0C0047F0       # the fatal routine writes {address, pc, code} here (internal SRAM)
TTB = 0x0A074000               # EKA1's one page directory (TTBR0 seen in every run)
REGNAMES = ['r%d' % i for i in range(13)] + ['sp', 'lr', 'pc']


def sdir(name):
    return ROOT / 'build/ngage' / ('sess-' + name)


# ---- gdb remote protocol (just what we need) ----
class Rsp:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port))
        self.buf = b''
        self.stop_reply = None

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.s.sendall(b'$' + data + b'#%02x' % (sum(data) & 0xff))

    def packet(self, timeout=None):
        """One packet payload (acks dropped), or None on timeout."""
        self.s.settimeout(timeout)
        try:
            while True:
                i = self.buf.find(b'$')
                j = self.buf.find(b'#', i)
                if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                    p = self.buf[i + 1:j]
                    self.buf = self.buf[j + 3:]
                    self.s.sendall(b'+')
                    return p.decode('latin1')
                b = self.s.recv(65536)
                if not b:
                    raise EOFError('gdb stub closed')
                self.buf += b
        except socket.timeout:
            return None
        finally:
            self.s.settimeout(None)

    def cmd(self, data):
        self.send(data)
        while True:
            p = self.packet(10)
            if p is None:
                raise TimeoutError('no reply to %r' % data)
            if p.startswith(('T', 'S')) and not str(data).startswith('?'):
                self.stop_reply = p          # an asynchronous stop that raced the command
                continue
            return p

    def reg(self, n):
        return struct.unpack('<I', bytes.fromhex(self.cmd('p%x' % n)[:8]))[0]

    def regs(self):
        r = {REGNAMES[i]: self.reg(i) for i in range(16)}
        r['cpsr'] = self.reg(0x19)
        return r

    def mem(self, va, n):
        out = b''
        while n > 0:
            k = min(n, 0x800)
            p = self.cmd('m%x,%x' % (va, k))
            if p.startswith('E'):
                raise ValueError('cannot read %08x' % va)
            out += bytes.fromhex(p)
            va += k; n -= k
        return out


# ---- daemon ----
class Daemon:
    def __init__(self, a):
        self.a = a
        self.d = sdir(a.session)
        self.state = 'starting'
        self.hit = None
        self.bps = {}                 # va -> label
        self.conds = {}               # va -> Python condition over the registers
        self.skipped = 0              # hits whose condition was false
        self.crashes = 0

    def log(self, *m):
        with open(self.d / 'daemon.log', 'a') as f:
            print(time.strftime('%H:%M:%S'), *m, file=f)

    def run(self):
        a, d = self.a, self.d
        s_ = socket.socket(); s_.bind(('127.0.0.1', 0)); port = s_.getsockname()[1]; s_.close()
        mach = 'ngage' + (',ppm=%s' % a.ppm if a.ppm else '') + (',isi=%s' % a.isi if a.isi else '') \
            + (',flash=%s' % a.flash if a.flash else '') + (',mmc=%s' % a.mmc if a.mmc else '')
        cmd = [str(nr.QEMU), '-M', mach, '-bios', a.rom, '-display', 'none',
               '-serial', 'file:%s' % (d / 'serial'), '-monitor', 'unix:%s,server,nowait' % (d / 'mon'),
               '-qmp', 'unix:%s,server,nowait' % (d / 'qmp'),
               '-d', a.debug, '-D', str(d / 'qemu.log'), '-gdb', 'tcp:127.0.0.1:%d' % port, '-S']
        self.q = subprocess.Popen(cmd, stdout=open(d / 'qemu.out', 'w'), stderr=subprocess.STDOUT)
        (d / 'qemu.pid').write_text(str(self.q.pid))
        for _ in range(100):
            try:
                self.rsp = Rsp(port); break
            except OSError:
                time.sleep(0.1)
        self.rsp.cmd('?')
        for c in filter(None, a.catch.split(',')):
            c, _, cond = c.partition('?')           # VA[=LABEL][?EXPR]: stop only if EXPR holds
            c, _, label = c.partition('=')          # (EXPR: Python over r0..r12, sp, lr, pc)
            va = CATCHES.get(c) if c in CATCHES else int(c, 16)
            self.set_bp(va, label or c)
            if cond:
                self.conds[va] = cond
        self.t0 = time.time()
        self.cont()
        ctl = socket.socket(socket.AF_UNIX); ctl.bind(str(d / 'ctl')); ctl.listen(4)
        while True:
            if self.q.poll() is not None:
                self.state = 'exited'
                self.log('qemu exited', self.q.returncode)
                break
            r, _, _ = select.select([ctl, self.rsp.s], [], [], 1.0)
            if self.rsp.s in r or (self.state == 'running' and self.rsp.stop_reply):
                self.on_stop()
            if ctl in r:
                c, _ = ctl.accept()
                try:
                    req = json.loads(c.makefile().readline())
                    res = self.handle(req)
                except Exception as e:  # report, keep the session
                    res = {'error': '%s: %s' % (type(e).__name__, e)}
                c.sendall((json.dumps(res) + '\n').encode()); c.close()
                if res.get('bye'):
                    break
        try:
            self.q.terminate(); self.q.wait(5)
        except Exception:
            self.q.kill()

    def set_bp(self, va, label):
        r = self.rsp.cmd('Z1,%x,4' % va)
        if r != 'OK':
            raise ValueError('breakpoint %08x: %s' % (va, r))
        self.bps[va] = label

    def cont(self):
        if self.state in ('frozen', 'paused'):
            pc = self.rsp.reg(15)
            if pc in self.bps:               # step over our own breakpoint, as gdb does
                self.rsp.cmd('z1,%x,4' % pc)
                self.rsp.send('s'); self.rsp.packet(10)
                self.rsp.cmd('Z1,%x,4' % pc)
        self.rsp.stop_reply = None
        self.rsp.send('c')
        self.state = 'running'

    def on_stop(self):
        p = self.rsp.stop_reply or self.rsp.packet(0.5)
        self.rsp.stop_reply = None
        if p is None or not p.startswith(('T', 'S')):
            return
        regs = self.rsp.regs()
        if self.bps.get(regs['pc'], '').startswith('~') and self.state == 'running':
            self.trace(regs)                    # a logging catch: record and go on
            self.state = 'frozen'
            self.cont()
            return
        cond = self.conds.get(regs['pc'])
        if cond and self.state == 'running':
            try:
                hold = eval(cond, {}, dict(regs))
            except Exception as e:
                hold = True
                self.log('condition %r: %s' % (cond, e))
            if not hold:
                self.skipped += 1
                self.state = 'frozen'           # cont() steps over a breakpoint only when stopped
                self.cont()
                return
        label = self.bps.get(regs['pc'], 'stopped')
        self.state = 'frozen' if regs['pc'] in self.bps else 'paused'
        self.hit = {'label': label, 'pc': regs['pc'], 'at_secs': round(time.time() - self.t0, 1)}
        self.log('stop', label, '%08x' % regs['pc'])
        if self.state == 'frozen' and regs['pc'] in (CATCHES['fatal'], CATCHES['pabt'],
                                                     CATCHES['dabt'], CATCHES['undef']):
            self.crashes += 1                   # crash catches only: a dump is 16 MB
            self.hit['dump'] = str(self.dump(self.d / ('crash-%d' % self.crashes), label))

    def trace(self, regs):
        """VA=~LABEL catches: time, label, thread, r0-r3, lr and the descriptor at r1 (if any)."""
        line = '%7.2f %s r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x' % (
            time.time() - self.t0, self.bps[regs['pc']][1:], regs['r0'], regs['r1'], regs['r2'],
            regs['r3'], regs['lr'])
        try:
            cur = struct.unpack('<I', self.rsp.mem(CUR_THREAD, 4))[0]
            line += ' [%s]' % self.thread_name(cur)
            w0 = struct.unpack('<I', self.rsp.mem(regs['r1'], 4))[0]
            if w0 >> 28 in (0, 3) and 0 < (w0 & 0xfff) < 128:       # TBufC / TBuf (16-bit)
                n = w0 & 0xfff
                b = self.rsp.mem(regs['r1'] + (4 if w0 >> 28 == 0 else 8), 2 * n)
                line += ' "%s"' % b.decode('utf-16-le', 'replace')
        except Exception:
            pass
        with open(self.d / 'trace.log', 'a') as f:
            print(line, file=f)

    def thread_name(self, obj):
        try:
            w = struct.unpack('<5I', self.rsp.mem(obj, 20))
            proc = struct.unpack('<5I', self.rsp.mem(w[2], 20))[4] if w[2] else 0

            def nm(hb):
                if not hb:
                    return '?'
                ln = struct.unpack('<I', self.rsp.mem(hb, 4))[0] & 0xfff   # TDesC16: chars
                return self.rsp.mem(hb + 4, 2 * min(ln, 64)).decode('utf-16-le', 'replace')
            return '%s::%s' % (nm(proc), nm(w[4]))
        except Exception:
            return '?'

    def summary(self):
        regs = self.rsp.regs()
        cur = struct.unpack('<I', self.rsp.mem(CUR_THREAD, 4))[0]
        out = {'state': self.state, 'hit': self.hit, 'regs': {k: '%08x' % v for k, v in regs.items()},
               'mode': '%02x' % (regs['cpsr'] & 0x1f), 'thread': '%08x %s' % (cur, self.thread_name(cur))}
        try:
            out['fault_words'] = ['%08x' % w for w in struct.unpack('<3I', self.rsp.mem(FAULT_WORDS, 12))]
        except Exception:
            pass
        name = nr.rom_namer()
        st = []
        try:
            words = struct.unpack('<64I', self.rsp.mem(regs['sp'], 256))
            for i, v in enumerate(words):
                if 0x5031af9c <= v < 0x51170000:
                    st.append('sp+%03x %08x %s' % (4 * i, v, name(v & ~1) or ''))
        except Exception as e:
            st.append(str(e))
        out['stack'] = st
        return out

    def dump(self, out, label):
        out.mkdir(parents=True, exist_ok=True)
        nr.monitor(self.d / 'mon', ['pmemsave 0x%x 0x%x "%s"' % (SDRAM, SDRAM_SIZE, out / 'sdram.bin'),
                                    'pmemsave 0x0c000000 0x8000 "%s"' % (out / 'isram.bin')])
        info = self.summary()
        info['label'] = label
        info['monitor_regs'] = nr.monitor(self.d / 'mon', ['info registers']).popitem()[1]
        (out / 'state.json').write_text(json.dumps(info, indent=1))
        return out

    def handle(self, req):
        op = req['op']
        if op == 'status':
            return {'state': self.state, 'hit': self.hit, 'skipped': self.skipped,
                    'bps': {'%08x' % k: v for k, v in self.bps.items()},
                    'uptime': round(time.time() - self.t0, 1)}
        if op == 'stop':
            return {'bye': True}
        if self.state == 'running' and op not in ('pause', 'break', 'del'):
            return {'error': 'running (pause first, or wait for a catch)'}
        if op == 'pause':
            if self.state == 'running':
                self.rsp.s.sendall(b'\x03')
                p = self.rsp.packet(5)
                self.rsp.stop_reply = p
                self.on_stop()
            return {'state': self.state, 'hit': self.hit}
        if op == 'summary':
            return self.summary()
        if op == 'regs':
            return {k: '%08x' % v for k, v in self.rsp.regs().items()}
        if op == 'x':
            b = self.rsp.mem(req['va'], 4 * req['n'])
            return {'words': ['%08x' % w for w in struct.unpack('<%dI' % req['n'], b)]}
        if op in ('break', 'del'):
            was = self.state == 'running'
            if was:
                self.rsp.s.sendall(b'\x03'); self.rsp.packet(5)
            if op == 'break':
                self.set_bp(req['va'], req.get('label') or 'bp')
            else:
                self.rsp.cmd('z1,%x,4' % req['va']); self.bps.pop(req['va'], None)
            if was:
                self.cont()
            return {'bps': {'%08x' % k: v for k, v in self.bps.items()}}
        if op == 'cont':
            self.cont()
            return {'state': self.state}
        if op == 'dump':
            return {'dump': str(self.dump(pathlib.Path(req['dir']), 'manual'))}
        return {'error': 'unknown op %s' % op}


class Qmp:
    """A QMP connection to a session's QEMU (screendump, input-send-event, ...)."""

    def __init__(self, name):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(str(sdir(name) / 'qmp'))
        self.f = self.s.makefile('rw')
        json.loads(self.f.readline())                  # greeting
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:          # skip asynchronous events
                return r


def call(name, req, timeout=60):
    c = socket.socket(socket.AF_UNIX); c.settimeout(timeout)
    c.connect(str(sdir(name) / 'ctl'))
    c.sendall((json.dumps(req) + '\n').encode())
    return json.loads(c.makefile().readline())


def show(res):
    if 'error' in res:
        print('error:', res['error']); sys.exit(1)
    if 'regs' in res and 'stack' in res:
        h = res.get('hit') or {}
        print('state %s  hit %s at pc %s after %s s' % (res['state'], h.get('label'), res['regs']['pc'], h.get('at_secs')))
        r = res['regs']
        print('  ' + ' '.join('%s=%s' % (k, r[k]) for k in REGNAMES[:8]))
        print('  ' + ' '.join('%s=%s' % (k, r[k]) for k in REGNAMES[8:]) + ' cpsr=%s mode=%s' % (r['cpsr'], res['mode']))
        print('  thread', res['thread'], ' fault words', ' '.join(res.get('fault_words', [])))
        if h.get('dump'):
            print('  dump', h['dump'])
        for l in res['stack']:
            print('   ', l)
        return
    print(json.dumps(res, indent=1))


# ---- offline: read a crash dump through the ARMv5 page tables ----
def dump_reader(dirp):
    dirp = pathlib.Path(dirp)
    ram = (dirp / 'sdram.bin').read_bytes()
    st = json.loads((dirp / 'state.json').read_text())
    ttb = TTB
    rom = (ROOT / 'eka1/output/rom/ROM.bin').read_bytes()

    def phys(pa, n):
        if SDRAM <= pa < SDRAM + SDRAM_SIZE:
            return ram[pa - SDRAM:pa - SDRAM + n]
        if pa < len(rom):
            return rom[pa:pa + n]
        raise ValueError('phys %08x not in the dump' % pa)

    def w(pa):
        return struct.unpack('<I', phys(pa, 4))[0]

    def tr(va):
        l1 = w(ttb + (va >> 20) * 4)
        if l1 & 3 == 2:
            return (l1 & 0xfff00000) | (va & 0xfffff)
        if l1 & 3 == 1:                                   # coarse
            l2 = w((l1 & 0xfffffc00) + ((va >> 12) & 0xff) * 4)
        elif l1 & 3 == 3:                                 # fine
            l2 = w((l1 & 0xfffff000) + ((va >> 10) & 0x3ff) * 4)
        else:
            raise ValueError('va %08x unmapped' % va)
        if l2 & 3 == 1:
            return (l2 & 0xffff0000) | (va & 0xffff)
        if l2 & 3 == 2:
            return (l2 & 0xfffff000) | (va & 0xfff)
        if l2 & 3 == 3:
            return (l2 & 0xfffffc00) | (va & 0x3ff)
        raise ValueError('va %08x unmapped' % va)

    def read(va, n):
        return b''.join(phys(tr(va + i), 4) for i in range(0, n, 4))[:n]
    return read


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-s', '--session', default='main')
    ap.add_argument('op')
    ap.add_argument('args', nargs='*')
    ap.add_argument('--catch', default='fatal,pabt,dabt')
    ap.add_argument('--debug', default='unimp,guest_errors')
    ap.add_argument('--rom', default=str(ROOT / 'eka1/output/rom/ROM.bin'))
    ap.add_argument('--ppm', default=str(ROOT / 'eka1/extracted/RH2941.026'))
    ap.add_argument('--flash', default='', help="the phone's flash file (kept across boots, Q55); default none")
    ap.add_argument('--mmc', default='', help='memory card image (E:), eka1/tools/ngage_card.py; default none')
    ap.add_argument('--isi', default=str(ROOT / 'src/qemu-ngage/isi-replies.txt'),
                    help='ISI reply table (Q50); "" = none')
    a = ap.parse_args()
    d = sdir(a.session)
    if a.op == '_daemon':
        dm = Daemon(a)
        try:
            dm.run()
        finally:                                # never leave an orphaned QEMU behind
            q = getattr(dm, 'q', None)
            if q and q.poll() is None:
                q.kill()
        return
    if a.op == 'start':
        if (d / 'ctl').exists():
            try:
                call(a.session, {'op': 'status'}, 2)
                sys.exit('session %s is running (stop it first)' % a.session)
            except OSError:
                pass
        d.mkdir(parents=True, exist_ok=True)
        for f in d.glob('*'):
            if f.is_file() or f.is_socket():
                f.unlink()
        args = [sys.executable, __file__, '-s', a.session, '_daemon', '--catch', a.catch,
                '--debug', a.debug, '--rom', a.rom, '--ppm', a.ppm, '--isi', a.isi,
                '--flash', os.path.abspath(a.flash) if a.flash else '',
                '--mmc', os.path.abspath(a.mmc) if a.mmc else '']
        subprocess.Popen(args, start_new_session=True, stdin=subprocess.DEVNULL,
                         stdout=open(d / 'daemon.out', 'w'), stderr=subprocess.STDOUT)
        for _ in range(100):
            if (d / 'ctl').exists():
                break
            time.sleep(0.1)
        else:
            sys.exit('session did not start:\n' + (d / 'daemon.out').read_text()[-1500:])
        show(call(a.session, {'op': 'status'}))
        return
    if a.op == 'dumpx':
        read = dump_reader(a.args[0])
        va, n = int(a.args[1], 16), int(a.args[2]) if len(a.args) > 2 else 16
        words = struct.unpack('<%dI' % n, read(va, 4 * n))
        for i in range(0, n, 4):
            print('%08x: %s' % (va + 4 * i, ' '.join('%08x' % x for x in words[i:i + 4])))
        return
    if a.op == 'wait':
        end = time.time() + (float(a.args[0]) if a.args else 300)
        while time.time() < end:
            st = call(a.session, {'op': 'status'})
            if st['state'] != 'running':
                break
            time.sleep(1)
        st = call(a.session, {'op': 'status'})
        if st['state'] in ('frozen', 'paused'):
            show(call(a.session, {'op': 'summary'}))
        else:
            show(st)
        return
    if a.op in ('objects', 'stack', 'shot', 'mon'):
        mon = d / 'mon'
        if a.op == 'objects':
            nr.print_objects(mon)
        elif a.op == 'stack':
            nr.print_stack(mon, a.args[0])
        elif a.op == 'shot':
            nr.monitor(mon, ['screendump %s -f png' % os.path.abspath(a.args[0])]); print('saved', a.args[0])
        else:
            for k, v in nr.monitor(mon, [' '.join(a.args)]).items():
                v = v.replace('(qemu)', '')
                print(v.split('\x1b[K')[-1].lstrip('\r\n').strip())   # drop the terminal echo
        return
    req = {'op': a.op}
    if a.op in ('x', 'break', 'del'):
        va, _, label = a.args[0].partition('=')
        req['va'] = CATCHES.get(va) if va in CATCHES else int(va, 16)
        req['label'] = label or va
        req['n'] = int(a.args[1]) if len(a.args) > 1 else 16
    if a.op == 'dump':
        req['dir'] = os.path.abspath(a.args[0]) if a.args else str(d / 'dump-manual')
    res = call(a.session, req)
    if a.op == 'x' and 'words' in res:
        w = res['words']
        for i in range(0, len(w), 4):
            print('%08x: %s' % (req['va'] + 4 * i, ' '.join(w[i:i + 4])))
        return
    show(res)


if __name__ == '__main__':
    main()
