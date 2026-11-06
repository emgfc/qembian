"""sydb daemon: owns one QEMU session of the virtual N95 and serves JSON requests.

One process per session dir (default build/sydb). It starts QEMU through scripts/run-modern.sh
(-name sydb, so syb_run/syb_gdb, which kill only 'sybtool' QEMUs, leave it alone) with
  serial0  -> stdout, timestamped into SESSION/serial.log (kernel debug port, RDebug)
  serial1  -> SESSION/agent.sock (devagent.exe on the phone, line protocol, see devagent.cpp)
  QMP      -> SESSION/qmp.sock (keys, screenshots, HMP)
  gdbstub  -> tcp port SESSION/gdb.port (persistent gdb, sydb dbg ...)
and listens on SESSION/ctl.sock for one JSON object per line: {"op": ..., ...} -> one JSON reply.
"""
import base64, hashlib, io, json, os, re, signal, socket, socketserver, subprocess, sys, threading, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHUNK = 2048                                           # = KAckChunk in devagent.cpp
IDLE_RE = re.compile(r'sets 101f8766/41 = 203')        # SysAp "self-test OK" = idle screen up
PANIC_RE = re.compile(r'^Thread (\S+) (Panic \S+ ?-?\d*|Killed -?\d+)')


def unesc(s):
    out, i = bytearray(), 0
    b = s.encode('latin-1')
    while i < len(b):
        if b[i] == 0x25 and i + 2 < len(b) + 0 and re.match(rb'[0-9A-Fa-f]{2}', b[i + 1:i + 3]):
            out.append(int(b[i + 1:i + 3], 16)); i += 3
        else:
            out.append(b[i]); i += 1
    return out.decode('latin-1')


def esc(s):
    return ''.join(c if ' ' < c < '\x7f' and c not in '%=' else '%%%02X' % (ord(c) & 0xff) for c in s)


def parse_fields(parts):
    d = {}
    for p in parts:
        if '=' in p:
            k, v = p.split('=', 1)
            v = unesc(v)
            if re.fullmatch(r'-?\d+', v):
                v = int(v)
            d[k] = v
    return d


class AgentError(Exception):
    pass


class AgentLink:
    """serial1 connection to devagent.exe; one request at a time, events queued."""

    def __init__(self, sess, path):
        self.sess, self.path = sess, path
        self.sock = None
        self.lock = threading.Lock()          # one request at a time
        self.cv = threading.Condition()
        self.reply = None                      # (tag, items, final) while a request runs
        self.tag = None
        self.raw_want = 0
        self.raw = bytearray()
        self.seq = 0
        self.hello = None
        self.go = False
        self.acked = None
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.sess.stopping:
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self.path)
            except OSError:
                time.sleep(0.3)
                continue
            self.sock = s
            buf = bytearray()
            try:
                while True:
                    data = s.recv(65536)
                    if not data:
                        break
                    buf += data
                    buf = self._consume(buf)
            except OSError:
                pass
            self.sock = None
            time.sleep(0.3)

    def _consume(self, buf):
        while buf:
            if self.raw_want:
                n = min(self.raw_want, len(buf))
                self.raw += buf[:n]
                del buf[:n]
                self.raw_want -= n
                continue
            i = buf.find(b'\n')
            if i < 0:
                return buf
            line = buf[:i].decode('latin-1').rstrip('\r')
            del buf[:i + 1]
            self._line(line)
        return buf

    def _line(self, line):
        parts = line.split(' ')
        if len(parts) < 2:
            return
        tag, kind = parts[0], parts[1]
        fields = parse_fields(parts[2:])
        if tag == '*':
            if kind == 'hello':
                self.hello = dict(fields, t=self.sess.now())
            self.sess.event('agent.' + kind, **fields)
            return
        with self.cv:
            if tag != self.tag:
                return
            if kind == '.':
                self.reply[0].append(fields)
            elif kind == 'go':
                self.go = True
            elif kind == 'ack':
                self.acked = fields.get('got')
            elif kind == 'data':
                self.raw = bytearray()
                self.raw_want = fields.get('size', 0)
                self.reply[1]['data'] = fields
            elif kind in ('ok', 'err'):
                self.reply[1].update(fields)
                self.reply[1]['_status'] = kind
            self.cv.notify_all()

    def connected(self):
        return self.sock is not None

    def request(self, cmd, args=(), timeout=30, payload=None, raw_args=None):
        if not self.sock:
            raise AgentError('agent link not connected (QEMU not running?)')
        with self.lock:
            self.seq += 1
            tag = 't%d' % self.seq
            items, final = [], {}
            with self.cv:
                self.tag, self.reply, self.go = tag, (items, final), False
            line = ' '.join([tag, cmd] + [esc(str(a)) for a in args])
            if raw_args:
                line += ' ' + raw_args
            self.sock.sendall(line.encode('latin-1') + b'\n')
            deadline = time.time() + timeout
            with self.cv:
                if payload is not None:
                    while not self.go and '_status' not in final:
                        if not self.cv.wait(max(0.01, deadline - time.time())) and time.time() > deadline:
                            raise AgentError('timeout waiting for go')
                    # flow control: CHUNK bytes, then wait for the agent's "ack" (got=N)
                    off = 0
                    while off < len(payload) and '_status' not in final:
                        n = min(CHUNK, len(payload) - off)
                        self.acked = None
                        self.cv.release()
                        try:
                            self.sock.sendall(payload[off:off + n])
                        finally:
                            self.cv.acquire()
                        off += n
                        if off < len(payload):
                            while self.acked is None and '_status' not in final:
                                if not self.cv.wait(10):
                                    raise AgentError('put: no ack at %d bytes' % off)
                            deadline = time.time() + timeout
                while '_status' not in final or self.raw_want:
                    if not self.cv.wait(max(0.01, deadline - time.time())) and time.time() > deadline:
                        raise AgentError('timeout (%s)' % cmd)
                self.tag = None
            if final.pop('_status') == 'err':
                raise AgentError('%s: error %s %s' % (cmd, final.get('code'), final.get('msg', '')))
            if 'data' in final:
                final['bytes'] = bytes(self.raw)
            return items, final


class Session:
    def __init__(self, sdir, rom, drives, gui, extra):
        self.sdir, self.rom, self.drives, self.gui, self.extra = sdir, rom, drives, gui, extra
        self.stopping = False
        self.t0 = time.time()
        self.events, self.ev_seq = [], 0
        self.ev_cv = threading.Condition()
        self.state = 'booting'
        self.last_shot_hash = None
        self.qemu = None
        self.log = open(os.path.join(sdir, 'serial.log'), 'w')
        self.log_lines = 0

    def now(self):
        return round(time.time() - self.t0, 3)

    def event(self, kind, **f):
        with self.ev_cv:
            self.ev_seq += 1
            e = dict(seq=self.ev_seq, t=self.now(), kind=kind, **f)
            self.events.append(e)
            del self.events[:-2000]
            self.ev_cv.notify_all()

    def start(self):
        sd, dd = self.sdir, self.drives
        for f in ('agent.sock', 'qmp.sock'):
            try:
                os.unlink(os.path.join(sd, f))
            except FileNotFoundError:
                pass
        s = socket.socket(); s.bind(('127.0.0.1', 0)); self.gdb_port = s.getsockname()[1]; s.close()
        open(os.path.join(sd, 'gdb.port'), 'w').write(str(self.gdb_port))
        cmd = [os.path.join(ROOT, 'scripts', 'run-modern.sh'), '--qmp', os.path.join(sd, 'qmp.sock'),
               '--name', 'sydb', '--nvmem', os.path.join(dd, 'nvmemory.img'),
               '--nvmem2', os.path.join(dd, 'nvmemory2.img'),
               '--dtb', os.path.join(ROOT, 'build', 'n95', 'syborg-n95.dtb')]
        if self.gui:
            cmd.append('--gui')
        cmd.append(os.path.abspath(self.rom))
        cmd += ['-serial', 'unix:%s,server=on,wait=off' % os.path.join(sd, 'agent.sock'),
                '-gdb', 'tcp:127.0.0.1:%d' % self.gdb_port,
                '-audiodev', 'wav,id=snd0,path=' + os.path.join(sd, 'audio.wav'), '-machine', 'audiodev=snd0',
                '-global', 'syborg-rtc.offset-file=' + os.path.join(dd, 'rtc-offset')] + self.extra
        self.qemu = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=open(os.path.join(sd, 'qemu.err'), 'w'),
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        self.t0 = time.time()
        threading.Thread(target=self._pump, daemon=True).start()
        self.agent = AgentLink(self, os.path.join(sd, 'agent.sock'))
        self.event('session.start', rom=self.rom)

    def _pump(self):
        for raw in self.qemu.stdout:
            line = raw.decode('latin-1').rstrip('\r\n')
            t = self.now()
            self.log.write('%.3f %s\n' % (t, line))
            self.log.flush()
            self.log_lines += 1
            if self.state == 'booting' and IDLE_RE.search(line):
                self.state = 'idle'
                self.event('phone.idle')
            m = PANIC_RE.match(line)
            if m:
                self.event('thread.' + ('panic' if m.group(2).startswith('Panic') else 'killed'),
                           thread=m.group(1), how=m.group(2))
        self.state = 'stopped'
        self.event('qemu.exit', code=self.qemu.wait())

    def stop(self):
        self.stopping = True
        if self.qemu and self.qemu.poll() is None:
            try:
                self.qmp('quit')
            except Exception:
                pass
            try:
                self.qemu.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(self.qemu.pid, signal.SIGKILL)

    # ---- QMP ----
    def qmp(self, name, **args):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(30)
        s.connect(os.path.join(self.sdir, 'qmp.sock'))
        f = s.makefile('rw')
        json.loads(f.readline())
        for c, a in (('qmp_capabilities', {}), (name, args)):
            f.write(json.dumps({'execute': c, 'arguments': a}) + '\n'); f.flush()
            while True:
                r = json.loads(f.readline())
                if 'event' in r:
                    continue
                if 'error' in r:
                    s.close()
                    raise AgentError('%s: %s' % (c, r['error'].get('desc')))
                break
        s.close()
        return r.get('return')

    # ---- log ----
    def log_query(self, grep=None, since=0.0, max_lines=50, tail=True):
        rx = re.compile(grep) if grep else None
        out = []
        with open(os.path.join(self.sdir, 'serial.log'), encoding='latin-1') as f:
            for line in f:
                try:
                    t = float(line.split(' ', 1)[0])
                except ValueError:
                    continue
                if t < since:
                    continue
                if rx and not rx.search(line):
                    continue
                out.append(line.rstrip('\n'))
        more = max(0, len(out) - max_lines)
        out = out[-max_lines:] if tail else out[:max_lines]
        return {'lines': out, 'more': more}

    def wait_log(self, regex, timeout, since=None):
        rx = re.compile(regex)
        since = self.now() if since is None else since
        end = time.time() + timeout
        while time.time() < end:
            r = self.log_query(regex, since, 1, tail=False)
            if r['lines']:
                return {'matched': True, 'line': r['lines'][0]}
            time.sleep(0.25)
        return {'matched': False}


def screenshot(sess, path=None, scale=1.0, crop=None, only_if_changed=False):
    tmp = os.path.join(sess.sdir, 'screen.png')
    sess.qmp('screendump', filename=tmp, format='png')
    data = open(tmp, 'rb').read()
    h = hashlib.sha1(data).hexdigest()[:12]
    changed = h != sess.last_shot_hash
    sess.last_shot_hash = h
    if only_if_changed and not changed:
        return {'changed': False, 'hash': h}
    out = path or os.path.join(sess.sdir, 'shots', time.strftime('%H%M%S') + '-%s.png' % h)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    if scale != 1.0 or crop:
        from PIL import Image
        im = Image.open(io.BytesIO(data))
        if crop:
            im = im.crop(tuple(crop))
        if scale != 1.0:
            im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))))
        im.save(out)
    else:
        open(out, 'wb').write(data)
    return {'changed': changed, 'hash': h, 'path': os.path.abspath(out)}


KEYMAP = {'lsk': 'f5', 'rsk': 'f7', 'ok': 'f6', 'select': 'f6', 'menu': 'f8', 'star': 'kp_multiply',
          'hash': 'f10', 'clear': 'backspace', 'back': 'f7'}


def press(sess, keys, delay=0.3):
    for k in keys:
        if isinstance(k, (int, float)):
            time.sleep(k)
            continue
        chord = [KEYMAP.get(p, p) for p in k.split('-')]
        sess.qmp('send-key', keys=[{'type': 'qcode', 'data': c} for c in chord])
        time.sleep(delay)


def handle(sess, req):
    op = req.get('op')
    a = sess.agent
    if op == 'status':
        return {'state': sess.state, 'uptime': sess.now(), 'agent': bool(a.connected()) and a.hello,
                'qemu_pid': sess.qemu.pid if sess.qemu else None, 'session': sess.sdir,
                'gdb_port': sess.gdb_port, 'log_lines': sess.log_lines, 'events': sess.ev_seq}
    if op == 'stop':
        threading.Thread(target=lambda: (time.sleep(0.2), sess.stop(), os._exit(0)), daemon=True).start()
        return {'ok': True}
    if op == 'agent':
        items, final = a.request(req['cmd'], req.get('args', []), req.get('timeout', 30),
                                 raw_args=req.get('raw'))
        return {'items': items, **final}
    if op == 'put':
        data = open(req['local'], 'rb').read() if 'local' in req else base64.b64decode(req['b64'])
        t = time.time()
        _, final = a.request('put', [req['remote'], len(data), '%08x' % (zlib.crc32(data) & 0xffffffff)],
                             timeout=60 + len(data) / 20000, payload=data)
        return dict(final, ms=int((time.time() - t) * 1000))
    if op == 'get':
        _, final = a.request('get', [req['remote']], timeout=120)
        data = final.pop('bytes', b'')
        if (zlib.crc32(data) & 0xffffffff) != int(str(final['data']['crc']), 16):
            raise AgentError('crc mismatch on get')
        if req.get('local'):
            open(req['local'], 'wb').write(data)
            return {'size': len(data), 'local': req['local']}
        return {'size': len(data), 'b64': base64.b64encode(data).decode()}
    if op == 'events':
        since, wait, kinds = req.get('since', 0), req.get('wait', 0), req.get('kinds')
        end = time.time() + wait
        with sess.ev_cv:
            while True:
                ev = [e for e in sess.events if e['seq'] > since and
                      (not kinds or any(e['kind'].startswith(k) for k in kinds))]
                if ev or time.time() >= end:
                    break
                sess.ev_cv.wait(end - time.time())
        return {'events': ev[-req.get('max', 50):], 'last': sess.ev_seq}
    if op == 'log':
        return sess.log_query(req.get('grep'), req.get('since', 0), req.get('max', 50), req.get('tail', True))
    if op == 'wait':
        what = req.get('what', 'idle')
        timeout = req.get('timeout', 120)
        end = time.time() + timeout
        if what == 'idle':
            while sess.state == 'booting' and time.time() < end:
                time.sleep(0.25)
            return {'state': sess.state, 't': sess.now()}
        if what == 'agent':
            while not (a.connected() and a.hello) and time.time() < end:
                time.sleep(0.25)
            # the hello may have been sent before we connected: ask
            if a.connected() and not a.hello:
                pass
            if a.connected():
                try:
                    _, f = a.request('hello', timeout=max(1, end - time.time()))
                    a.hello = a.hello or dict(f, t=sess.now())
                except AgentError as e:
                    return {'agent': False, 'error': str(e)}
            return {'agent': bool(a.hello), 'hello': a.hello, 't': sess.now()}
        return sess.wait_log(what, timeout, req.get('since'))
    if op == 'key':
        press(sess, req['keys'], req.get('delay', 0.3))
        return {'ok': True}
    if op == 'type':
        sys.path.insert(0, os.path.join(ROOT, 'tools'))
        import qmp as qmpmod
        for ch in req['text']:
            if ch.isalnum():
                keys = ['shift', ch.lower()] if ch.isupper() else [ch]
            elif ch in qmpmod.SHIFTED:
                keys = ['shift', qmpmod.SHIFTED[ch]]
            else:
                keys = [qmpmod.PLAIN.get(ch, ch)]
            sess.qmp('send-key', keys=[{'type': 'qcode', 'data': k} for k in keys])
            time.sleep(req.get('delay', 0.15))
        return {'ok': True}
    if op == 'screenshot':
        return screenshot(sess, req.get('path'), req.get('scale', 1.0), req.get('crop'), req.get('only_if_changed', False))
    if op == 'dbg':
        import debugger
        if not hasattr(sess, 'dbg'):
            sess.dbg = debugger.Debugger(sess)
        try:
            return sess.dbg.op(req)
        except debugger.DbgError as e:
            raise AgentError(str(e))
    if op == 'hmp':
        return {'out': sess.qmp('human-monitor-command', **{'command-line': req['cmd']})}
    raise AgentError('unknown op %r' % op)


def serve(sdir, rom, drives, gui, extra):
    sess = Session(sdir, rom, drives, gui, extra)
    sess.start()
    path = os.path.join(sdir, 'ctl.sock')
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

    class H(socketserver.StreamRequestHandler):
        def handle(self):
            for line in self.rfile:
                try:
                    req = json.loads(line)
                    res = {'ok': True, **handle(sess, req)}
                except AgentError as e:
                    res = {'ok': False, 'error': str(e)}
                except Exception as e:
                    res = {'ok': False, 'error': '%s: %s' % (type(e).__name__, e)}
                self.wfile.write((json.dumps(res) + '\n').encode())
                self.wfile.flush()

    class S(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
        daemon_threads = True

    srv = S(path, H)
    open(os.path.join(sdir, 'daemon.pid'), 'w').write(str(os.getpid()))

    def watch():
        sess.qemu.wait()
        time.sleep(0.5)
        srv.shutdown()
    threading.Thread(target=watch, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *a: (sess.stop(), os._exit(0)))
    srv.serve_forever()
    sess.stop()
