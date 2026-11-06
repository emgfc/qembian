"""Persistent gdb (MI2) for a sydb session: QEMU gdbstub, kernel DWARF, app ELFs, ROM symbols.

All-stop: a stop freezes the whole phone (agent requests time out meanwhile, QMP still works).
Stop records are small dicts: reason, pc/lr symbolised (gdb symbols, else tools/syb_sym ROM
names), current thread name (kernel DWARF: TheScheduler.iCurrentThread), up to 8 frames.
"""
import os, queue, re, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ksyms

CODE_RE = re.compile(r'N95CODE: (\S+) code=([0-9a-f]+)\+([0-9a-f]+) data=([0-9a-f]+)\+([0-9a-f]+) bss=([0-9a-f]+)')
THREAD_EXPR = ('((DThread*)((unsigned)TheScheduler.iCurrentThread - (unsigned)&((DThread*)0)->iNThread))')


class DbgError(Exception):
    pass


def mi_parse(s):
    """Parse an MI result tail (k=v,...) into python values."""
    pos = 0

    def value():
        nonlocal pos
        c = s[pos]
        if c == '"':
            pos += 1
            out = []
            while s[pos] != '"':
                if s[pos] == '\\':
                    pos += 1
                    out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\'}.get(s[pos], s[pos]))
                else:
                    out.append(s[pos])
                pos += 1
            pos += 1
            return ''.join(out)
        if c in '{[':
            end = '}' if c == '{' else ']'
            pos += 1
            items, d = [], {}
            while s[pos] != end:
                m = re.match(r'([\w-]+)=', s[pos:])
                if m:
                    pos += m.end()
                    v = value()
                    if c == '{':
                        d[m.group(1)] = v
                    else:
                        items.append(v)
                else:
                    items.append(value())
                if s[pos] == ',':
                    pos += 1
            pos += 1
            return d if c == '{' else items
        raise ValueError('bad MI at %d: %r' % (pos, s[pos:pos + 20]))

    out = {}
    while pos < len(s):
        m = re.match(r'([\w-]+)=', s[pos:])
        if not m:
            break
        pos += m.end()
        out[m.group(1)] = value()
        if pos < len(s) and s[pos] == ',':
            pos += 1
    return out


class Debugger:
    def __init__(self, sess):
        self.sess = sess
        self.p = None
        self.token = 0
        self.results = {}
        self.cv = threading.Condition()
        self.stops = queue.Queue()
        self.running = True
        self.console = []
        self.loaded = {}             # module name -> elf
        self.thread_filter = None
        self.rom = None
        self.catcher = None           # catcher.Catcher: owns the catch breakpoints (sydb catch)
        self.catch_q = queue.Queue()

    # ---- process / MI plumbing ----
    def start(self):
        if self.p and self.p.poll() is None:
            return
        self.p = subprocess.Popen(['gdb', '-nx', '-q', '--interpreter=mi2'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._catch_worker, daemon=True).start()
        self.mi('-gdb-set mi-async on')       # accept commands (interrupt) while the target runs
        for c in ('set pagination off', 'set confirm off', 'set architecture arm', 'set width 0',
                  'set print pretty off', ksyms.add_symbol_file()):
            self.cli(c, timeout=120)
        self.mi('-target-select remote 127.0.0.1:%d' % self.sess.gdb_port, timeout=30)
        self.running = False          # attaching stops the CPU

    def _reader(self):
        for line in self.p.stdout:
            line = line.rstrip('\n')
            m = re.match(r'^(\d*)\^(\w+),?(.*)$', line)
            if m:
                tok = int(m.group(1)) if m.group(1) else None
                with self.cv:
                    self.results[tok] = (m.group(2), m.group(3))
                    self.cv.notify_all()
                if m.group(2) == 'running':
                    self.running = True
                continue
            if line.startswith('*running'):
                self.running = True
            elif line.startswith('*stopped'):
                self.running = False
                st = mi_parse(line[len('*stopped,'):]) if ',' in line else {}
                # stops at catch breakpoints are the catcher's (handled on its own thread, because
                # handling issues gdb commands whose results this reader thread must deliver)
                if self.catcher and self.catcher.owns(st):
                    self.catch_q.put(st)
                else:
                    self.stops.put(st)
            elif line.startswith('~'):
                with self.cv:
                    self.console.append(mi_parse('x=' + line[1:])['x'])

    def mi(self, cmd, timeout=30):
        with self.cv:
            self.token += 1
            tok = self.token
            self.console = []
        self.p.stdin.write('%d%s\n' % (tok, cmd))
        self.p.stdin.flush()
        end = time.time() + timeout
        with self.cv:
            while tok not in self.results:
                if not self.cv.wait(max(0.01, end - time.time())) and time.time() > end:
                    raise DbgError('gdb timeout: ' + cmd)
            cls, rest = self.results.pop(tok)
            console = ''.join(self.console)
        if cls == 'error':
            raise DbgError(mi_parse(rest).get('msg', rest))
        r = mi_parse(rest) if rest else {}
        r['_console'] = console
        return r

    def cli(self, cmd, timeout=30):
        return self.mi('-interpreter-exec console "%s"' % cmd.replace('\\', '\\\\').replace('"', '\\"'),
                       timeout)['_console']

    # ---- helpers ----
    def romsym(self, addr):
        try:
            if self.rom is None:
                import syb_sym
                self.rom = syb_sym.Rom()
            return self.rom.describe(addr, short=True)
        except Exception:
            return None

    def describe(self, addr):
        try:
            out = self.cli('info symbol 0x%x' % addr).strip()
            if out and not out.startswith('No symbol'):
                return out.split(' in section')[0]
        except DbgError:
            pass
        return self.romsym(addr) or '0x%x' % addr

    def reg(self, name):
        return int(self.cli('p/x $%s' % name).split('=')[-1].strip(), 16)

    def _catch_worker(self):
        while True:
            st = self.catch_q.get()
            try:
                self.catcher.handle(st)
            except Exception as e:           # never leave the phone frozen because of a bug here
                self.sess.event('catch.error', error='%s: %s' % (type(e).__name__, e))
                try:
                    if not self.running:
                        self.mi('-exec-continue')
                except Exception:
                    pass

    # ---- memory / expression helpers (target stopped) ----
    def read_mem(self, addr, n):
        out = b''
        while n > 0:
            k = min(n, 4096)
            r = self.mi('-data-read-memory-bytes 0x%x %d' % (addr, k))
            mem = r.get('memory') or []
            chunk = bytes.fromhex(mem[0]['contents']) if mem else b''
            out += chunk
            if len(chunk) < k:
                break
            addr += k; n -= k
        return out

    def u32(self, addr):
        b = self.read_mem(addr, 4)
        return int.from_bytes(b, 'little') if len(b) == 4 else None

    def evalx(self, expr):
        return int(self.cli('p/x (unsigned)(%s)' % expr).split('=')[-1].strip(), 16)

    def read_des(self, addr, wide):
        """Symbian descriptor (TDesC8/TDesC16) at addr -> str"""
        w = self.u32(addr)
        if w is None:
            return '?'
        n, typ = w & 0x0fffffff, w >> 28
        if typ == 0:   data = addr + 4                    # EBufC
        elif typ == 1: data = self.u32(addr + 4)          # EPtrC
        elif typ == 2: data = self.u32(addr + 8)          # EPtr
        elif typ == 3: data = addr + 8                    # EBuf
        elif typ == 4: data = (self.u32(addr + 8) or 0) + 4  # EBufCPtr -> TBufC
        else: return '?'
        n = min(n, 256)
        raw = self.read_mem(data, n * (2 if wide else 1))
        return raw.decode('utf-16-le' if wide else 'latin-1', 'replace')

    def thread_name(self, dthread=None):
        try:
            t = ('0x%x' % dthread) if dthread else self.cli('p/x (unsigned)' + THREAD_EXPR).split('=')[-1].strip()
            names = []
            for e in ('iOwningProcess->iName', 'iName'):
                p = int(self.cli('p/x (unsigned)((DThread*)%s)->%s' % (t, e)).split('=')[-1].strip(), 16)
                if not p:
                    names.append('?'); continue
                n = int(self.cli('p/x *(unsigned*)0x%x' % p).split('=')[-1].strip(), 16) & 0xfff
                raw = self.cli('x/%dcb 0x%x' % (min(n, 64), p + 8))
                names.append(''.join(re.findall(r"'(.)'", raw)) or '?')
            return '%s::%s' % tuple(names)
        except (DbgError, ValueError):
            return '?'

    def stop_record(self, st):
        pc, lr, cpsr = self.reg('pc'), self.reg('lr'), self.reg('cpsr')
        rec = {'reason': st.get('reason', 'stopped'), 'pc': '0x%08x' % pc, 'at': self.describe(pc),
               'lr': self.describe(lr), 'mode': {0x10: 'usr', 0x13: 'svc', 0x12: 'irq', 0x11: 'fiq', 0x17: 'abt',
                                                  0x1b: 'und', 0x1f: 'sys'}.get(cpsr & 0x1f, '?'),
               'thumb': bool(cpsr & 0x20), 'thread': self.thread_name()}
        if st.get('bkptno'):
            rec['bp'] = int(st['bkptno'])
        try:
            bt = self.cli('bt 8').strip().splitlines()
            rec['frames'] = [re.sub(r'\s+', ' ', l) for l in bt[:8]]
        except DbgError:
            pass
        return rec

    # ---- API (called by daemon.handle) ----
    def op(self, req):
        sub = req.get('sub')
        if sub == 'attach':
            self.start()
            return {'attached': True, 'stopped': True, **self.stop_record({'reason': 'attach'})}
        if not self.p or self.p.poll() is not None:
            raise DbgError('not attached (dbg attach)')
        if sub == 'detach':
            try:
                if self.running:
                    self.mi('-exec-interrupt', timeout=5)
                    self.wait(5)
                self.mi('-target-detach', timeout=10)    # QEMU resumes the guest
            except DbgError:
                pass
            try:
                self.p.terminate(); self.p.wait(5)
            except Exception:
                self.p.kill()
            self.p = None
            return {'detached': True}
        if sub in ('continue', 'step', 'next', 'stepi', 'nexti', 'finish'):
            if self.running and sub != 'continue':
                raise DbgError('target is running (dbg interrupt first)')
            self.thread_filter = req.get('thread') or self.thread_filter
            cmd = {'continue': '-exec-continue', 'step': '-exec-step', 'next': '-exec-next',
                   'stepi': '-exec-step-instruction', 'nexti': '-exec-next-instruction',
                   'finish': '-exec-finish'}[sub]
            while not self.stops.empty():
                self.stops.get_nowait()
            if not self.running:
                self.mi(cmd)
            if sub == 'continue' and not req.get('wait'):
                return {'running': True}
            return self.wait(req.get('wait', 10))
        if sub == 'wait':
            return self.wait(req.get('timeout', 30))
        if sub == 'interrupt':
            if self.running:
                try:
                    self.mi('-exec-interrupt', timeout=5)
                except DbgError:
                    import signal
                    self.p.send_signal(signal.SIGINT)
                return self.wait(10)
            return {'stopped': True, **self.stop_record({'reason': 'already stopped'})}
        if self.running:
            raise DbgError('target is running (dbg interrupt first)')
        if sub == 'break':
            loc = req['loc']
            if re.fullmatch(r'0x[0-9a-fA-F]+', loc):
                loc = '*' + loc
            r = self.mi('-break-insert %s%s %s' % ('-h ' if req.get('hw', True) else '',
                                                  '-t' if req.get('once') else '', loc))
            b = r.get('bkpt', {})
            if req.get('thread'):
                self.thread_filter = req['thread']
            addr = b.get('addr', '')
            if not re.fullmatch(r'0x[0-9a-fA-F]+', addr):           # <MULTIPLE>: list the locations
                locs = [l.get('addr') for l in (b.get('locations') or []) if isinstance(l, dict)]
                return {'bp': int(b.get('number', 0)), 'addr': locs or addr,
                        'at': [self.describe(int(a, 16)) for a in locs[:8] if a and a.startswith('0x')]}
            return {'bp': int(b.get('number', 0)), 'addr': addr, 'at': b.get('func') or self.describe(int(addr, 16))}
        if sub == 'delete':
            self.mi('-break-delete %s' % req.get('bp', ''))
            return {'deleted': req.get('bp', 'all')}
        if sub == 'regs':
            out = self.cli('info registers')
            regs = dict(re.findall(r'^(\w+)\s+(0x[0-9a-f]+)', out, re.M))
            return {'regs': regs, 'at': self.describe(int(regs.get('pc', '0'), 16))}
        if sub == 'mem':
            addr, n = int(str(req['addr']), 0), int(req.get('len', 64))
            r = self.mi('-data-read-memory-bytes 0x%x %d' % (addr, min(n, 4096)))
            return {'addr': '0x%x' % addr, 'hex': r['memory'][0]['contents'] if r.get('memory') else ''}
        if sub == 'write':
            addr = int(str(req['addr']), 0)
            self.mi('-data-write-memory-bytes 0x%x %s' % (addr, req['hex']))
            return {'written': len(req['hex']) // 2}
        if sub == 'eval':
            return {'value': self.cli('p ' + req['expr']).split('=', 1)[-1].strip()}
        if sub == 'cmd':                 # raw gdb console command (escape hatch)
            return {'out': self.cli(req['cmd'], timeout=req.get('timeout', 30))[-4000:]}
        if sub == 'bt':
            return {'frames': self.cli('bt %d' % req.get('n', 12)).strip().splitlines()}
        if sub == 'symbols':
            return self.load_symbols(req['name'], req.get('elf'))
        if sub == 'where':
            return self.stop_record({'reason': 'where'})
        raise DbgError('unknown dbg sub-command %r' % sub)

    def wait(self, timeout):
        end = time.time() + timeout
        while True:
            try:
                st = self.stops.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                return {'running': True, 'timeout': True}
            rec = self.stop_record(st)
            flt = self.thread_filter
            if flt and flt.lower() not in rec['thread'].lower() and st.get('reason') == 'breakpoint-hit' \
                    and time.time() < end:
                self.mi('-exec-continue')        # other thread hit the address: keep going
                continue
            return {'stopped': True, **rec}

    def load_symbols(self, name, elf=None):
        """Place our ELF at the address the kernel loaded it (N95CODE line in the serial log)."""
        r = self.sess.log_query(r'N95CODE: \S*%s' % re.escape(name), 0, 1)
        if not r['lines']:
            raise DbgError('no N95CODE line for %s (not loaded yet?)' % name)
        m = CODE_RE.search(r['lines'][-1])
        code, data, bss = int(m.group(2), 16), int(m.group(4), 16), int(m.group(6), 16)
        if not elf:
            base = os.path.basename(m.group(1)).rsplit('.', 1)[0].lower()
            for d in ('build/n95apps/obj-%s' % base, 'build/n95apps/obj-devagent'):
                cand = os.path.join(ROOT, d, base + '.elf')
                if os.path.exists(cand):
                    elf = cand
                    break
        if not elf or not os.path.exists(elf):
            raise DbgError('ELF for %s not found; pass elf=' % name)
        if name in self.loaded:
            try:
                self.cli('remove-symbol-file %s' % self.loaded[name])
            except DbgError:
                pass
        args = 'add-symbol-file %s -s .text 0x%x' % (elf, code)
        if data:
            args += ' -s .data 0x%x' % data
        if bss:
            args += ' -s .bss 0x%x' % bss
        self.cli(args, timeout=60)
        self.loaded[name] = elf
        return {'elf': elf, 'text': '0x%x' % code, 'data': '0x%x' % data}
