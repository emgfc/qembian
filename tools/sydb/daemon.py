"""sydb daemon: owns one QEMU session of the virtual N95 and serves JSON requests.

One process per session dir (default build/sydb). It starts QEMU through scripts/run-modern.sh
(-name sydb, so syb_run/syb_gdb, which kill only 'sybtool' QEMUs, leave it alone) with
  serial0  -> stdout, timestamped into SESSION/serial.log (kernel debug port, RDebug)
  serial1  -> SESSION/agent.sock (devagent.exe on the phone, line protocol, see devagent.cpp)
  QMP      -> SESSION/qmp.sock (keys, screenshots, HMP)
  gdbstub  -> tcp port SESSION/gdb.port (persistent gdb, sydb dbg ...)
and listens on SESSION/ctl.sock for one JSON object per line: {"op": ..., ...} -> one JSON reply.
"""
import shutil, base64, hashlib, io, json, os, re, signal, socket, socketserver, subprocess, sys, threading, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHUNK = 2048                                           # = KAckChunk in devagent.cpp
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel                               # tools/device.py: the device profile (docs/devices.md)
DEVICE = devmodel.load(os.environ.get('SYBORG_DEVICE'))  # $SYBORG_DEVICE or devices/default
IDLE_RE = re.compile(re.escape(devmodel.get(DEVICE, 'milestones.idle', 'sets 101f8766/41 = 203')))   # SysAp self-test OK = idle
MSGAC_RE = re.compile(r'^MsgAc: M:(-?\d+) (\S+)->(\S+)')
MSGCO_RE = re.compile(r'^MsgCo: M:(-?\d+) r:(-?\d+) (\S+)->(\S+)')
KIPC = 1 << 11
IPCRD_RE = re.compile(r'^IPCRD (\S+) <- (\S+): "(.*)"$')
_FSOPS = None


def fs_op_name(fn):
    """F32 client opcode -> name, from the file server's OperationArray (index = opcode)."""
    global _FSOPS
    if _FSOPS is None:
        try:
            src = open(os.path.join(ROOT, 'build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'userlibandfileserver',
                                    'fileserver', 'sfile', 'sf_ops.h'), encoding='latin-1').read()
            body = src[src.index('OperationArray'):]
            _FSOPS = re.findall(r'\{\s*(EFs\w+)\s*,', body)
        except (OSError, ValueError):
            _FSOPS = []
    fn &= 0xffff                        # high bits carry flags (e.g. KIpcSlot...)
    return _FSOPS[fn][3:] if fn < len(_FSOPS) else None
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


def stamp_version(data):
    """New module version (minor = time-based counter) + header CRC for an E32 image, so the
    loader does not reuse a lingering code segment of an older build with the same name."""
    import struct
    if len(data) < 0x9c or data[0x10:0x14] != b'EPOC':
        return data
    d = bytearray(data)
    n = struct.unpack_from('<I', d, 0x64)[0]               # iCodeOffset = header size
    ver = struct.unpack_from('<I', d, 0x18)[0]
    # minor in 1..0x7fff: the loader refuses (KErrCorrupt) a minor version with the top bit set
    struct.pack_into('<I', d, 0x18, (ver & 0xffff0000) | (int(time.time() * 10) % 0x7fff + 1))
    struct.pack_into('<I', d, 0x14, 0xc90fdaa2)
    struct.pack_into('<I', d, 0x14, ~zlib.crc32(bytes(d[:n]), 0xffffffff) & 0xffffffff)
    return bytes(d)


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
    def __init__(self, sdir, rom, drives, gui, extra, restore=None, audio='wav', gl='host'):
        self.sdir, self.rom, self.drives, self.gui, self.extra = sdir, rom, drives, gui, extra
        self.audio, self.gl = audio, gl
        self.rom_mtime = os.path.getmtime(rom) if os.path.exists(rom) else 0   # phone: stale ROM check
        self.restore = restore         # snapshot dir to resume from (sydb fork / restore)
        self.stopping = False
        self.t0 = time.time()
        self.events, self.ev_seq = [], 0
        self.ev_cv = threading.Condition()
        self.state = 'booting'
        self.last_shot_hash = None
        self.qemu = None
        self.log = open(os.path.join(sdir, 'serial.log'), 'w')
        self.log_lines = 0
        self.ipc_match = None          # IPC trace filter (substring of client or server name)

    def now(self):
        return round(time.time() - self.t0, 3)

    def event(self, kind, **f):
        with self.ev_cv:
            self.ev_seq += 1
            e = dict(seq=self.ev_seq, t=self.now(), kind=kind, **f)
            self.events.append(e)
            del self.events[:-2000]
            self.ev_cv.notify_all()

    def audiodev(self):
        # wav: sound to <session>/audio.wav (unattended runs); host: the speakers, as run-gui does
        if self.audio == 'wav':
            return 'wav,id=snd0,path=' + os.path.join(self.sdir, 'audio.wav')
        drv = os.environ.get('AUDIO', 'pipewire') if self.audio == 'host' else self.audio
        return drv + ',id=snd0'

    def ram_path(self):
        # guest RAM as a shared file: sydb phone maps it and reads the frame buffer directly
        # (no VNC encoding); /dev/shm, so it costs nothing beyond the RAM QEMU allocates anyway
        import hashlib
        return '/dev/shm/sydb-%s.ram' % hashlib.md5(os.path.abspath(self.sdir).encode()).hexdigest()[:10]

    def start(self):
        sd, dd = self.sdir, self.drives
        try:
            os.unlink(self.ram_path())
        except FileNotFoundError:
            pass
        for f in ('agent.sock', 'qmp.sock', 'vnc.sock'):
            try:
                os.unlink(os.path.join(sd, f))
            except FileNotFoundError:
                pass
        s = socket.socket(); s.bind(('127.0.0.1', 0)); self.gdb_port = s.getsockname()[1]; s.close()
        open(os.path.join(sd, 'gdb.port'), 'w').write(str(self.gdb_port))
        cmd = [os.path.join(ROOT, 'scripts', 'run-modern.sh'), '--qmp', os.path.join(sd, 'qmp.sock'),
               '--name', 'sydb', '--nvmem', os.path.join(dd, 'nvmemory.img'),
               '--nvmem2', os.path.join(dd, 'nvmemory2.img'),
               '--device', DEVICE['_id'], '--dtb', devmodel.paths(DEVICE)['dtb']]
        if self.gui:
            cmd.append('--gui')
        cmd.append(os.path.abspath(self.rom))
        cmd += ['-serial', 'unix:%s,server=on,wait=off' % os.path.join(sd, 'agent.sock'),
                '-gdb', 'tcp:127.0.0.1:%d' % self.gdb_port,
                '-vnc', 'unix:' + os.path.join(sd, 'vnc.sock'),     # the screen for sydb phone
                '-audiodev', self.audiodev(), '-machine', 'audiodev=snd0',
                '-object', 'memory-backend-file,id=ram0,size=%dM,share=on,mem-path=%s'
                % (devmodel.get(DEVICE, 'board.ram_mb', 168), self.ram_path()),
                '-machine', 'memory-backend=ram0',
                # GLES: host GPU (syborg-gl, default) or the guest's software rasteriser
                '-global', 'syborg-gl.backend=' + ('off' if self.gl == 'soft' else 'host'),
                '-global', 'syborg-rtc.offset-file=' + os.path.join(dd, 'rtc-offset'),
                # snapshots may be taken while gdb holds the machine ('debug'), a run state an incoming
                # machine cannot enter: do not carry the run state; restores start paused (-S)
                '-global', 'migration.store-global-state=off'] + self.extra
        if self.restore:              # resume a saved machine, paused (-S): sydb resume / dbg attach
            cmd += ['-incoming', 'file:' + os.path.join(self.restore, 'state.bin'), '-S']
            self.state = 'restored'
        self.qemu = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=open(os.path.join(sd, 'qemu.err'), 'w'),
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        self.t0 = time.time()
        threading.Thread(target=self._pump, daemon=True).start()
        self.agent = AgentLink(self, os.path.join(sd, 'agent.sock'))
        self.event('session.start', rom=self.rom, restored=self.restore)

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
            if self.ipc_match is not None and line.startswith('Msg'):
                m = MSGAC_RE.match(line) or MSGCO_RE.match(line)
                if m and any(self.ipc_match.lower() in x.lower() for x in m.groups()[-2:]):
                    fn = int(m.group(1))
                    if line.startswith('MsgAc'):
                        srv = m.group(3)
                        e = dict(fn=fn, client=m.group(2), server=srv)
                        if fn in (-1, -2):
                            e['op'] = 'Connect' if fn == -1 else 'Disconnect'
                        elif 'FileServer' in srv and fs_op_name(fn):
                            e['op'] = fs_op_name(fn)
                        self.event('ipc.call', **e)
                    else:          # MsgCo: completing (server) thread -> client
                        self.event('ipc.done', fn=fn, r=int(m.group(2)), server=m.group(3), client=m.group(4))
            elif self.ipc_match is not None and line.startswith('IPCRD'):
                m = IPCRD_RE.match(line)
                if m and any(self.ipc_match.lower() in x.lower() for x in m.groups()[:2]):
                    self.event('ipc.read', server=m.group(1), client=m.group(2), data=m.group(3))
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
        try:
            os.unlink(self.ram_path())
        except FileNotFoundError:
            pass

    # ---- machine snapshots ("fork"): RAM + device state by QMP migrate to a file, drives copied ----
    def snapshot(self, name=None, keep_stopped=False):
        name = name or time.strftime('snap-%H%M%S')
        if not re.fullmatch(r'[\w.-]+', name):
            raise AgentError('snapshot name: letters, digits, . _ - only')
        d = os.path.join(self.sdir, 'snaps', name)
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
        before = self.qmp('query-status')['status']
        if before == 'running':
            self.qmp('stop')
        t0 = time.time()
        self.qmp('migrate', uri='file:' + os.path.join(d, 'state.bin'))
        while True:
            m = self.qmp('query-migrate')
            if m.get('status') in ('completed', 'failed', 'cancelled'):
                break
            if time.time() - t0 > 120:
                raise AgentError('snapshot: migration timed out')
            time.sleep(0.1)
        if m.get('status') != 'completed':
            raise AgentError('snapshot: migration %s: %s' % (m.get('status'), m.get('error-desc', '')))
        for f in ('nvmemory.img', 'nvmemory2.img', 'rtc-offset'):
            src = os.path.join(self.drives, f)
            if os.path.exists(src):
                subprocess.run(['cp', '--reflink=auto', '--sparse=always', src, os.path.join(d, f)], check=True)
        # a completed migration leaves the block devices inactive (handed over to the destination);
        # this machine goes on, so take them back (QMP cont would too, gdb's continue does not)
        try:
            self.qmp('blockdev-set-active', active=True)
        except Exception as e:
            self.event('snap.warning', error='blockdev-set-active: %s' % e)
        meta = {'name': name, 'session': self.sdir, 'rom': os.path.abspath(self.rom), 'uptime': self.now(),
                'created': time.strftime('%Y-%m-%d %H:%M:%S'), 'state_before': before,
                'seconds': round(time.time() - t0, 2), 'bytes': os.path.getsize(os.path.join(d, 'state.bin'))}
        json.dump(meta, open(os.path.join(d, 'meta.json'), 'w'), indent=1)
        if before == 'running' and not keep_stopped:
            self.qmp('cont')
        self.event('snap.saved', name=name, path=d)
        return dict(meta, path=d)

    def gc(self, keep_auto=5, keep_dumps=200, forks=True, dry=False):
        """retention: automatic snapshots (catch-*, auto-*) beyond the newest keep_auto, catch dumps
        beyond the newest keep_dumps, and fork session dirs whose daemon is not running. Snapshots
        with other names (made by hand) are kept until removed."""
        removed, freed = [], 0

        def size(p):
            n = 0
            for dp, _, fs in os.walk(p):
                for f in fs:
                    try:
                        n += os.lstat(os.path.join(dp, f)).st_blocks * 512
                    except OSError:
                        pass
            return n

        def rm(p):
            nonlocal freed
            freed += size(p) if os.path.isdir(p) else os.lstat(p).st_blocks * 512
            removed.append(os.path.relpath(p, ROOT))
            if not dry:
                shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)

        root = os.path.join(self.sdir, 'snaps')
        if os.path.isdir(root):
            auto = sorted((os.path.getmtime(os.path.join(root, n)), n) for n in os.listdir(root)
                          if re.match(r'(catch|auto)-', n))
            for _, n in auto[:-keep_auto] if keep_auto else auto:
                rm(os.path.join(root, n))
        cdir = os.path.join(self.sdir, 'catches')
        if os.path.isdir(cdir):
            ids = sorted({f.split('-', 1)[0] for f in os.listdir(cdir) if f[:3].isdigit()})
            for hid in ids[:-keep_dumps] if keep_dumps else ids:
                for f in os.listdir(cdir):
                    if f.startswith(hid + '-'):
                        rm(os.path.join(cdir, f))
        if forks:
            for n in os.listdir(os.path.join(ROOT, 'build')):
                p = os.path.join(ROOT, 'build', n)
                if n.startswith('sydb-fork-') and os.path.isdir(p) and os.path.abspath(p) != os.path.abspath(self.sdir):
                    pid = None
                    try:
                        pid = int(open(os.path.join(p, 'daemon.pid')).read())
                        os.kill(pid, 0)
                        continue            # running fork: keep
                    except (OSError, ValueError):
                        rm(p)
        return {'removed': removed, 'allocated_mb': round(freed / 1048576, 1), 'dry_run': dry,
                'note': 'allocated_mb counts reflinked drive blocks shared with the live drives, so it overstates what is freed'}

    def snapshots(self):
        root = os.path.join(self.sdir, 'snaps')
        out = []
        for n in sorted(os.listdir(root)) if os.path.isdir(root) else []:
            try:
                out.append(json.load(open(os.path.join(root, n, 'meta.json'))) | {'path': os.path.join(root, n)})
            except Exception:
                pass
        return out

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


# key names -> QEMU qcodes: the device keymap (devices/<id>/keymap.toml via tools/device.py)
KEYMAP = devmodel.key_aliases(DEVICE)


def press(sess, keys, delay=0.3):
    for k in keys:
        if isinstance(k, (int, float)):
            time.sleep(k)
            continue
        hold = None
        if ':' in k:                      # KEY:SECONDS = hold (long press), e.g. menu:1.5
            k, hold = k.rsplit(':', 1)
            hold = float(hold)
        chord = [KEYMAP.get(p, p) for p in k.split('-')]
        args = {'keys': [{'type': 'qcode', 'data': c} for c in chord]}
        if hold:
            args['hold-time'] = int(hold * 1000)
        sess.qmp('send-key', **args)
        time.sleep(delay + (hold or 0))


def handle(sess, req):
    op = req.get('op')
    a = sess.agent
    if op == 'status':
        return {'state': sess.state, 'uptime': sess.now(), 'agent': bool(a.connected()) and a.hello,
                'qemu_pid': sess.qemu.pid if sess.qemu else None, 'session': sess.sdir,
                'gdb_port': sess.gdb_port, 'log_lines': sess.log_lines, 'events': sess.ev_seq,
                'ram': sess.ram_path(), 'rom': os.path.abspath(sess.rom), 'rom_mtime': sess.rom_mtime,
                'device': DEVICE['_id']}
    if op == 'stop':
        threading.Thread(target=lambda: (time.sleep(0.2), sess.stop(), os._exit(0)), daemon=True).start()
        return {'ok': True}
    if op == 'agent':
        items, final = a.request(req['cmd'], req.get('args', []), req.get('timeout', 30),
                                 raw_args=req.get('raw'))
        return {'items': items, **final}
    if op == 'put':
        data = open(req['local'], 'rb').read() if 'local' in req else base64.b64decode(req['b64'])
        if req.get('stamp'):
            data = stamp_version(data)
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
    if op == 'trace':
        # trace ipc: switch the kernel's KIPC debug bit through the agent, turn matching
        # MsgAc/MsgCo lines into ipc.call / ipc.done events
        _, f = a.request('dmask', [])
        mask = int(str(f['mask']), 16)
        if req.get('on', True):
            sess.ipc_match = req.get('match', '')
            mask |= KIPC
        else:
            sess.ipc_match = None
            if not req.get('keep_kernel_bit'):
                mask &= ~KIPC
        _, f = a.request('dmask', ['set', '%x' % mask])
        return {'ipc': sess.ipc_match is not None, 'match': sess.ipc_match, 'mask': f['mask'], 'seq': sess.ev_seq}
    if op == 'dbg':
        import debugger
        if not hasattr(sess, 'dbg'):
            sess.dbg = debugger.Debugger(sess)
        try:
            return sess.dbg.op(req)
        except debugger.DbgError as e:
            raise AgentError(str(e))
    if op == 'catch':                     # freeze/dump/snapshot on thread death (catcher.py)
        import debugger, catcher
        if not hasattr(sess, 'dbg'):
            sess.dbg = debugger.Debugger(sess)
        if not getattr(sess, 'catcher', None):
            sess.catcher = catcher.Catcher(sess, sess.dbg)
        c, sub = sess.catcher, req.get('sub', 'list')
        try:
            if sub == 'add':
                return {'rule': c.add(req), **c.status()}
            if sub == 'rm':
                c.remove(req.get('id'))
                return c.status()
            if sub == 'list':
                return c.status()
            if sub == 'hits':
                return {'hits': c.hits[-int(req.get('max', 50)):]}
            if sub == 'show':
                h = [x for x in c.hits if x['id'] == int(req['id'])]
                if not h:
                    raise AgentError('no hit %s' % req['id'])
                full = json.load(open(h[0]['files']['json']))
                full.pop('log_tail', None) if req.get('brief') else None
                return {'hit': full, 'text': open(h[0]['files']['txt']).read()}
            if sub == 'resume':
                return c.resume()
        except (ValueError, debugger.DbgError) as e:
            raise AgentError(str(e))
        raise AgentError('catch: unknown sub %r' % sub)
    if op == 'snap':
        sub = req.get('sub', 'list')
        if sub == 'save':
            return sess.snapshot(req.get('name'), keep_stopped=req.get('stopped', False))
        if sub == 'list':
            return {'snapshots': sess.snapshots()}
        if sub == 'rm':
            p = os.path.join(sess.sdir, 'snaps', req['name'])
            if not os.path.isdir(p):
                raise AgentError('no snapshot %s' % req['name'])
            shutil.rmtree(p)
            return {'removed': req['name']}
        raise AgentError('snap: unknown sub %r' % sub)
    if op == 'gc':
        return sess.gc(int(req.get('keep_auto', 5)), int(req.get('keep_dumps', 200)), req.get('forks', True),
                       req.get('dry', False))
    if op == 'vm':                        # pause / resume the whole machine (not while gdb holds it)
        sub = req.get('sub', 'status')
        if sub == 'pause':
            sess.qmp('stop')
        elif sub == 'resume':
            sess.qmp('cont')
            if sess.state == 'restored':
                sess.state = 'idle'
        return {'status': sess.qmp('query-status')['status'], 'state': sess.state}
    if op == 'keyev':                     # key down or up (sydb phone: real presses, long presses)
        keys = [KEYMAP.get(k, k) for k in req['keys']]
        sess.qmp('input-send-event', events=[{'type': 'key', 'data': {'down': bool(req['down']),
                                              'key': {'type': 'qcode', 'data': k}}} for k in keys])
        return {'ok': True}
    if op == 'qmp':                       # raw QMP command (e.g. input-send-event)
        return {'ok': True, 'return': sess.qmp(req['cmd'], **(req.get('args') or {}))}
    if op == 'hmp':
        return {'out': sess.qmp('human-monitor-command', **{'command-line': req['cmd']})}
    raise AgentError('unknown op %r' % op)


def serve(sdir, rom, drives, gui, extra, restore=None, audio='wav', gl='host'):
    sess = Session(sdir, rom, drives, gui, extra, restore, audio, gl)
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
