#!/usr/bin/env python3
"""sydb: adb-like dev/debug bridge for the virtual N95 (see docs/sydb.md).

  sydb start [--seed DIR] [--fresh] [--gui] [--rom ROM] [--audio wav|host|DRIVER]   boot a session, wait for idle
  sydb stop | status | wait idle|agent|REGEX [--timeout S]
  sydb install | uninstall           agent on/off C: incl. its Starter startup-list record (session stopped)
  sydb ls PATH | push LOCAL REMOTE | pull REMOTE LOCAL | rm PATH | mkdir PATH
  sydb run PATH [ARGS...] [--wait S] | rerun LOCAL.exe [ARGS...] [--wait S] | kill PID|NAME
  sydb ps [PATTERN] | threads [PATTERN] | apps [FILTER] | launch UID|CAPTION | mem
  sydb startup add|rm|check PATH [w0 w1 w2] | startup list   (Starter's dynamic startup list)
  sydb wg | fg WGID | unlock | lock | keepawake on|off|unlocked
  sydb prop get CAT KEY | prop set CAT KEY VALUE              (hex category/key)
  sydb key KEY... [--delay S] | type TEXT | screenshot [OUT.png] [--scale F] [--changed]
  sydb log [--grep RE] [--since T] [--max N] [--head] | events [--since SEQ] [--wait S] [--kind K]
  sydb update-agent                  push a rebuilt devagent.exe/dafs.exe and restart the agent live
  sydb dbg attach|break LOC [--thread T]|continue [--wait S]|wait|step|next|stepi|finish|regs|bt|
           mem ADDR [LEN]|write ADDR HEX|eval EXPR|symbols NAME [ELF]|interrupt|detach|cmd GDBCMD
  sydb strace MATCH [--secs S]       IPC messages to/from processes matching MATCH (kernel KIPC trace)
  sydb tui                           curses view: status, window groups, events, log, command line
  sydb phone [--scale 2]             emulator window: screen, phone keys (long presses), reset/pause/
                                     screenshot/snapshot, slider (starts a session if none runs)
  sydb catch add --at ADDR [--thread RE] [--max N] [--action dump|freeze]   dump each time ADDR runs
  sydb catch add [--kind panic|kill|terminate|exit|any] [--thread RE] [--category CAT] [--reason N]
                 [--action dump|freeze] [--snapshot] [--once]   catch thread deaths (see docs/sydb.md)
  sydb catch list | hits | show ID | rm [ID] | resume
  sydb snap save [NAME] [--stopped] | snap list | snap rm NAME     machine snapshots (RAM+devices+drives)
  sydb fork SNAP [--into DIR] [--run] [--gui]   resume a snapshot in a new session (paused by default)
  sydb restore SNAP [--run]          the same into this session  | sydb pause | resume
  sydb gc [--keep-auto 5] [--keep-dumps 200] [--no-forks] [--dry-run]   retention (also after each catch hit)
  sydb qmp CMD [JSON-ARGS] | hmp CMD | mcp (MCP stdio server) | --json (raw JSON output) | --session DIR
Keys: QKeyCode names or aliases lsk rsk ok menu star hash clear back call end power volup voldown; 1.5 or 10 = pause in seconds (1 = key 1);
      KEY:SECONDS = hold (long press), e.g. menu:1.5 opens the task switcher.
"""
import argparse, json, os, shutil, socket, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel  # tools/device.py: build paths of the device ($SYBORG_DEVICE / devices/default)
DEVPATHS = devmodel.paths(devmodel.load())
sys.path.insert(0, HERE)
DEFAULT_SESSION = os.path.join(ROOT, 'build', 'sydb')
AGENT_EXE = os.path.join(ROOT, 'build', 'n95apps', 'devagent.exe')
DAFS_EXE = os.path.join(ROOT, 'build', 'n95apps', 'dafs.exe')
AGENT_PATH = 'c:\\sys\\bin\\devagent.exe'
import startlist


class Client:
    def __init__(self, sdir):
        self.sdir = sdir

    def alive(self):
        try:
            self.call({'op': 'status'}, timeout=3)
            return True
        except (OSError, RuntimeError):
            return False

    def call(self, req, timeout=None):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout or 600)
        s.connect(os.path.join(self.sdir, 'ctl.sock'))
        s.sendall((json.dumps(req) + '\n').encode())
        f = s.makefile('r')
        line = f.readline()
        s.close()
        if not line:
            raise RuntimeError('daemon closed the connection')
        return json.loads(line)


def die(msg):
    print('sydb: ' + msg, file=sys.stderr)
    sys.exit(1)


def need(r):
    if not r.get('ok'):
        die(r.get('error', 'failed'))
    return r


def drive_put(drives, local, remote_dir, name=None):
    env = dict(os.environ, DRIVE_C=os.path.join(drives, 'nvmemory.img'))
    d = ''
    for part in remote_dir.strip('/').split('/'):       # mkdir -p (errors = already there)
        d += '/' + part
        subprocess.run([os.path.join(ROOT, 'scripts', 'n95-drive.sh'), 'mkdir', 'c', d], env=env, capture_output=True, stdin=subprocess.DEVNULL)
    args = [os.path.join(ROOT, 'scripts', 'n95-drive.sh'), 'put', 'c', local, remote_dir]
    r = subprocess.run(args, env=env, capture_output=True, text=True)
    if r.returncode:
        die('n95-drive.sh failed: ' + (r.stderr or r.stdout)[-500:])
    if name and os.path.basename(local) != name:
        r = subprocess.run(['mren', '-o', '-i', os.path.join(drives, 'nvmemory.img'),
                            '::' + remote_dir.rstrip('/') + '/' + os.path.basename(local),
                            '::' + remote_dir.rstrip('/') + '/' + name], capture_output=True, text=True)
        if r.returncode:
            die('mren failed: ' + r.stderr[-300:])


def cmd_install(sdir, a):
    drives = os.path.join(sdir, 'drives')
    if Client(sdir).alive():
        die('stop the session first (the C: image is in use)')
    if not os.path.exists(os.path.join(drives, 'nvmemory.img')):
        die('no drives in %s yet: run "sydb start --seed DIR" (or --fresh) once' % drives)
    for f in (AGENT_EXE, DAFS_EXE):
        if not os.path.exists(f):
            die('%s missing: run src/n95apps/devagent/build.sh' % f)
    drive_put(drives, AGENT_EXE, '/sys/bin')
    drive_put(drives, DAFS_EXE, '/sys/bin')
    drive_rm(drives, '/sys/bin/HALSettings.exe')        # the old autostart hack, if present
    # autostart: a record in Starter's dynamic startup list (what a SIS install registers)
    data = drive_read(drives, '/' + startlist.PATH)
    drive_write(drives, '/' + startlist.PATH,
                startlist.add(data, AGENT_PATH, open(AGENT_EXE, 'rb').read()))
    print('installed devagent.exe + dafs.exe on C: and registered devagent in Starter\'s startup list')


def cmd_uninstall(sdir, a):
    drives = os.path.join(sdir, 'drives')
    if Client(sdir).alive():
        die('stop the session first, or use "sydb startup rm" while it runs')
    data = drive_read(drives, '/' + startlist.PATH)
    if data:
        new = startlist.remove(data, AGENT_PATH)
        if new:
            drive_write(drives, '/' + startlist.PATH, new)
        else:
            drive_rm(drives, '/' + startlist.PATH)
    for f in ('devagent.exe', 'dafs.exe', 'HALSettings.exe'):
        drive_rm(drives, '/sys/bin/' + f)
    print('removed devagent from the startup list and devagent.exe/dafs.exe from C:')


def mt(drives, *args):
    # mtools asks on a name clash (mmd of an existing directory waited forever for an answer):
    # never give it a terminal, and let mmd skip existing directories
    clash = ['-D', 's'] if args[0] == 'mmd' else []
    return subprocess.run(list(args[:1]) + clash + ['-i', os.path.join(drives, 'nvmemory.img')] + list(args[1:]),
                          capture_output=True, stdin=subprocess.DEVNULL, timeout=120)


def drive_read(drives, path):
    r = mt(drives, 'mtype', '::' + path)
    return r.stdout if r.returncode == 0 else None


def drive_write(drives, path, data):
    tmp = os.path.join(drives, '.sydb-tmp')
    open(tmp, 'wb').write(data)
    d = os.path.dirname(path)
    acc = ''
    for part in d.strip('/').split('/'):
        acc += '/' + part
        mt(drives, 'mmd', '::' + acc)
    r = mt(drives, 'mcopy', '-o', tmp, '::' + path)
    os.unlink(tmp)
    if r.returncode:
        die('mcopy %s failed: %s' % (path, r.stderr.decode()[-300:]))


def drive_rm(drives, path):
    mt(drives, 'mdel', '::' + path)


DEVICE = {'nvmemory.img': 'drive-c.img', 'nvmemory2.img': 'drive-e.img', 'rtc-offset': 'rtc-offset'}
DEVICE_SESSION = os.path.join(ROOT, 'build', 'sydb-device')
DEVICE_ROM = os.path.join(DEVPATHS['dir'], 'hybrid-exp.img')   # the owner's run-gui ROM (sound on)


def use_device(drives):
    # --device: the session runs on scripts/run-gui.sh's phone in place (build/n95/drive-c.img,
    # drive-e.img, rtc-offset), as links in <session>/drives
    os.makedirs(drives, exist_ok=True)
    for link, f in DEVICE.items():
        src = os.path.join(DEVPATHS['dir'], f)
        if f.endswith('.img'):
            if not os.path.exists(src):
                die('%s missing: boot once with scripts/run-gui.sh' % src)
            if subprocess.run(['pgrep', '-f', 'qemu-system-arm.*' + os.path.realpath(src)],
                              stdout=subprocess.DEVNULL).returncode == 0:
                die('%s is in use by a running QEMU (run-gui?): close it first' % src)
        dst = os.path.join(drives, link)
        if os.path.islink(dst) or os.path.exists(dst):
            if os.path.islink(dst) and os.path.realpath(dst) == os.path.realpath(src):	# old links name build/n95/...
                continue
            die('%s exists and is not a link to the run-gui device: use another --session' % dst)
        os.symlink(src, dst)


def cmd_start(sdir, a):
    c = Client(sdir)
    if c.alive():
        print('session already running in %s' % sdir)
        return
    drives = os.path.join(sdir, 'drives')
    os.makedirs(sdir, exist_ok=True)
    if getattr(a, 'device', False):
        use_device(drives)
    if a.fresh and os.path.isdir(drives):
        shutil.rmtree(drives)
    if a.seed:
        if os.path.isdir(drives):
            shutil.rmtree(drives)
        shutil.copytree(a.seed, drives)
    os.makedirs(drives, exist_ok=True)
    if a.seed or a.fresh or a.install:
        if os.path.exists(os.path.join(drives, 'nvmemory.img')):
            cmd_install(sdir, a)
    rom = os.path.abspath(a.rom)
    log = open(os.path.join(sdir, 'daemon.log'), 'w')
    subprocess.Popen([sys.executable, os.path.abspath(__file__), '--session', sdir, '_daemon', rom, drives,
                      '--audio', getattr(a, 'audio', None) or 'wav', '--gl', getattr(a, 'gl', None) or 'host']
                     + (['--gui'] if a.gui else []), stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                     start_new_session=True, cwd=ROOT)
    for _ in range(100):
        time.sleep(0.1)
        if c.alive():
            break
    else:
        die('daemon did not come up, see %s' % os.path.join(sdir, 'daemon.log'))
    if a.no_wait:
        print('started; session %s' % sdir)
        return
    r = need(c.call({'op': 'wait', 'what': 'idle', 'timeout': a.timeout}))
    s = 'phone %s at %.1f s' % (r['state'], r['t'])
    r = c.call({'op': 'wait', 'what': 'agent', 'timeout': 30})
    s += '; agent ' + ('up (pid %s)' % r['hello'].get('pid') if r.get('agent') else 'not responding')
    print(s)


def find_snapshot(sdir, name):
    for cand in (name, os.path.join(sdir, 'snaps', name), os.path.join(DEFAULT_SESSION, 'snaps', name)):
        if os.path.isfile(os.path.join(cand, 'state.bin')):
            return os.path.abspath(cand)
    die('no snapshot %r (sydb snap list)' % name)


def cmd_fork(sdir, cmd, rest):
    """fork: resume a snapshot in a new session dir (default build/sydb-fork-NAME), paused.
    restore: the same into this session (stops it first)."""
    ap = argparse.ArgumentParser(prog='sydb ' + cmd)
    ap.add_argument('snapshot'); ap.add_argument('--into'); ap.add_argument('--gui', action='store_true')
    ap.add_argument('--run', action='store_true', help='resume at once instead of staying paused')
    a = ap.parse_args(rest)
    snap = find_snapshot(sdir, a.snapshot)
    meta = json.load(open(os.path.join(snap, 'meta.json')))
    if cmd == 'fork':
        target = os.path.abspath(a.into or os.path.join(ROOT, 'build', 'sydb-fork-' + os.path.basename(snap)))
    else:
        target = sdir
        c = Client(sdir)
        if c.alive():
            c.call({'op': 'stop'})
            for _ in range(100):
                time.sleep(0.1)
                if not c.alive():
                    break
    c = Client(target)
    if c.alive():
        die('session %s is running; stop it first' % target)
    drives = os.path.join(target, 'drives')
    os.makedirs(drives, exist_ok=True)
    for f in ('nvmemory.img', 'nvmemory2.img', 'rtc-offset'):
        if os.path.exists(os.path.join(snap, f)):
            subprocess.run(['cp', '--reflink=auto', '--sparse=always', os.path.join(snap, f), os.path.join(drives, f)], check=True)
    log = open(os.path.join(target, 'daemon.log'), 'w')
    subprocess.Popen([sys.executable, os.path.abspath(__file__), '--session', target, '_daemon', meta['rom'], drives,
                      '--restore', snap] + (['--gui'] if a.gui else []), stdout=log, stderr=log,
                     stdin=subprocess.DEVNULL, start_new_session=True, cwd=ROOT)
    for _ in range(100):
        time.sleep(0.1)
        if c.alive():
            break
    else:
        die('daemon did not come up, see %s' % os.path.join(target, 'daemon.log'))
    for _ in range(100):                 # wait for the incoming migration to finish loading
        try:
            st = c.call({'op': 'vm', 'sub': 'status'})
            if st.get('status') in ('paused', 'prelaunch', 'running'):
                break
        except Exception:
            pass
        time.sleep(0.2)
    if a.run:
        c.call({'op': 'vm', 'sub': 'resume'})
    print(json.dumps({'session': target, 'snapshot': snap, 'uptime_at_snapshot': meta.get('uptime'),
                      'state': 'running' if a.run else 'paused (sydb --session %s resume | dbg attach)' % target}))


def fmt_items(items, keys):
    for it in items:
        print('  '.join(str(it.get(k, '')) for k in keys if k in it or True).rstrip())


def main():
    argv = sys.argv[1:]
    js = '--json' in argv
    if js:
        argv.remove('--json')
    sdir = DEFAULT_SESSION
    if '--session' in argv:
        i = argv.index('--session'); sdir = os.path.abspath(argv[i + 1]); del argv[i:i + 2]
    elif '--device' in argv:          # your run-gui phone's session, for every command
        sdir = DEVICE_SESSION
        if argv and argv[0] not in ('start', 'install', 'phone'):
            argv.remove('--device')
    if not argv or argv[0] in ('-h', '--help', 'help'):
        print(__doc__); return
    cmd, rest = argv[0], argv[1:]

    if cmd == '_daemon':
        import daemon
        ap = argparse.ArgumentParser(); ap.add_argument('rom'); ap.add_argument('drives')
        ap.add_argument('--gui', action='store_true'); ap.add_argument('--restore')
        ap.add_argument('--audio', default='wav'); ap.add_argument('--gl', default='host')
        x = ap.parse_args(rest)
        daemon.serve(sdir, x.rom, x.drives, x.gui, [], x.restore, x.audio, x.gl)
        return
    if cmd == 'phone':                 # emulator window: screen + phone keys + machine controls
        ap = argparse.ArgumentParser(prog='sydb phone')
        ap.add_argument('--scale', type=float, default=2.0)
        ap.add_argument('--audio', default='host', help='sound of a session it starts: host (speakers, default), wav, or a QEMU audiodev driver')
        ap.add_argument('--device', action='store_true', help="your run-gui phone (drives in place, session build/sydb-device)")
        ap.add_argument('--rom', help='ROM of a session it starts (default hybrid.img, with --device hybrid-exp.img)')
        ap.add_argument('--install', action='store_true', help='install/update the sydb agent on its C: first')
        ap.add_argument('--keep', action='store_true', help='leave the session running when the window closes')
        ap.add_argument('--restart', action='store_true', help='restart a running session first (fresh boot, current ROM)')
        ap.add_argument('--gl', choices=['host', 'soft'], default='host', help='GLES on the host GPU (default) or the software rasteriser')
        x = ap.parse_args(rest)
        if x.device and sdir == DEFAULT_SESSION:
            sdir = DEVICE_SESSION
        c0 = Client(sdir)
        if c0.alive():
            st = c0.call({'op': 'status'})
            rom = st.get('rom')
            stale = rom and os.path.exists(rom) and os.path.getmtime(rom) > (st.get('rom_mtime') or 0) + 1
            if x.restart or stale:
                print('session %s %s: restarting it' % (sdir, 'runs an older build of ' + os.path.basename(rom)
                                                        if stale else 'is running'), flush=True)
                x.rom = x.rom or rom                 # the same ROM file, now its current build
                c0.call({'op': 'stop'})
                for _ in range(150):
                    if not c0.alive():
                        break
                    time.sleep(0.1)
            else:
                print('joining the running session %s (up %.0f s; --restart for a fresh boot)' % (sdir, st.get('uptime', 0)), flush=True)
        started = False
        if not Client(sdir).alive():
            subprocess.run([sys.executable, os.path.abspath(__file__), '--session', sdir, 'start', '--no-wait',
                            '--audio', x.audio, '--gl', x.gl] + (['--device'] if x.device else []) + (['--rom', x.rom] if x.rom else [])
                           + (['--install'] if x.install else []), check=True)
            started = True
        import phone
        rc = phone.main(sdir, x.scale)
        # like closing an emulator: a session this window started ends with it (not one it joined)
        if started and not x.keep and Client(sdir).alive():
            print('stopping the session %s (--keep leaves it running)' % sdir)
            Client(sdir).call({'op': 'stop'})
        sys.exit(rc)
    if cmd == 'tui':
        import tui
        tui.main(sdir)
        return
    if cmd == 'mcp':
        import mcp
        mcp.serve(sdir)
        return
    if cmd == 'uninstall':
        cmd_uninstall(sdir, None)
        return
    if cmd in ('fork', 'restore'):
        cmd_fork(sdir, cmd, rest)
        return
    if cmd in ('start', 'install'):
        ap = argparse.ArgumentParser(prog='sydb ' + cmd)
        ap.add_argument('--seed'); ap.add_argument('--fresh', action='store_true')
        ap.add_argument('--gui', action='store_true'); ap.add_argument('--install', action='store_true')
        ap.add_argument('--no-wait', action='store_true'); ap.add_argument('--timeout', type=float, default=180)
        ap.add_argument('--gl', choices=['host', 'soft'], default='host', help='GLES on the host GPU (syborg-gl, default) or the software rasteriser')
        ap.add_argument('--device', action='store_true', help="run on scripts/run-gui.sh's phone (build/n95/drive-c.img, "
                        'drive-e.img, rtc-offset) in place; ROM default build/n95/hybrid-exp.img')
        ap.add_argument('--audio', default='wav', help='wav (<session>/audio.wav, default), host (speakers: '
                        '$AUDIO or pipewire, like run-gui) or any QEMU audiodev driver (pa, alsa, sdl, none)')
        ap.add_argument('--rom')
        a = ap.parse_args(rest)
        a.rom = a.rom or (DEVICE_ROM if a.device else DEVPATHS['rom'])
        (cmd_start if cmd == 'start' else cmd_install)(sdir, a)
        return

    c = Client(sdir)
    if not c.alive():
        die('no session running (sydb start)')

    def out(r, text=None):
        need(r)
        if js:
            print(json.dumps(r, indent=1))
        elif text:
            text(r)
        else:
            r = {k: v for k, v in r.items() if k != 'ok'}
            print(json.dumps(r) if r else 'ok')

    def agent(name, args=(), raw=None, timeout=30):
        return c.call({'op': 'agent', 'cmd': name, 'args': list(args), 'raw': raw, 'timeout': timeout})

    if cmd == 'status':
        out(c.call({'op': 'status'}))
    elif cmd == 'stop':
        r = c.call({'op': 'stop'})
        for _ in range(150):               # return once the daemon is gone, so a start can follow
            if not c.alive():
                break
            time.sleep(0.1)
        out(r)
    elif cmd == 'wait':
        ap = argparse.ArgumentParser(); ap.add_argument('what'); ap.add_argument('--timeout', type=float, default=120)
        x = ap.parse_args(rest)
        out(c.call({'op': 'wait', 'what': x.what, 'timeout': x.timeout}))
    elif cmd == 'ls':
        out(agent('ls', [rest[0] if rest else 'C:\\']),
            lambda r: [print('%-4s %9s  %s  %s' % ('d' if i.get('type') == 'dir' else '-', i.get('size', ''),
                                                    i.get('mtime', ''), i.get('name'))) for i in r['items']])
    elif cmd == 'push':
        out(c.call({'op': 'put', 'local': os.path.abspath(rest[0]), 'remote': rest[1]}),
            lambda r: print('%d bytes in %d ms' % (r['size'], r['ms'])))
    elif cmd == 'pull':
        out(c.call({'op': 'get', 'remote': rest[0], 'local': os.path.abspath(rest[1])}),
            lambda r: print('%d bytes -> %s' % (r['size'], r['local'])))
    elif cmd in ('rm', 'mkdir'):
        out(agent(cmd, rest[:1]))
    elif cmd in ('run', 'rerun'):
        ap = argparse.ArgumentParser(); ap.add_argument('path'); ap.add_argument('args', nargs='*')
        ap.add_argument('--wait', type=float, default=0)
        x, unknown = ap.parse_known_args(rest)
        x.args += unknown
        path = x.path
        if cmd == 'rerun':
            name = os.path.basename(path)
            remote = 'C:\\sys\\bin\\' + name
            r = agent('ps', [name.rsplit('.', 1)[0] + '*'])
            for it in r.get('items', []):
                if it.get('exit') == 'pending':
                    agent('kill', [it['pid']])
            ev0 = c.call({'op': 'events', 'since': 0})['last']
            need(c.call({'op': 'put', 'local': os.path.abspath(path), 'remote': remote, 'stamp': True}))
            path = remote
        ev0 = c.call({'op': 'events', 'since': 0})['last']
        r = need(agent('run', [path], raw=' '.join(x.args) or None))
        res = {'pid': r.get('pid'), 'name': r.get('name')}
        if x.wait:
            end = time.time() + x.wait
            while time.time() < end:
                e = c.call({'op': 'events', 'since': ev0, 'wait': end - time.time(), 'kinds': ['agent.exit']})
                ev0 = e['last']
                hit = [v for v in e['events'] if v.get('pid') == r.get('pid')]
                if hit:
                    res['exit'] = {k: hit[0][k] for k in ('type', 'reason', 'category') if k in hit[0]}
                    break
        print(json.dumps(res))
    elif cmd == 'kill':
        out(agent('kill', rest[:2]))
    elif cmd in ('ps', 'threads'):
        out(agent(cmd, rest[:1]), lambda r: [print(
            '%-10s %-8s %s%s' % (i.get('pid', i.get('tid', '')), i.get('exit', ''), i.get('name'),
                                 ('  %s %s' % (i.get('category', ''), i.get('reason', '')) if i.get('exit') not in ('pending', None) else '')))
            for i in r['items']])
    elif cmd == 'apps':
        flt = rest[0].lower() if rest else ''
        out(agent('apps', timeout=60), lambda r: [print('%-10s %-24s %s' % (i['uid'], i.get('caption', ''), i.get('path', '')))
                                                for i in r['items'] if flt in (str(i.get('caption', '')) + str(i.get('path', ''))).lower()])
    elif cmd == 'launch':
        out(agent('launch', [' '.join(rest)], timeout=60))
    elif cmd == 'wg':
        out(agent('wg'), lambda r: [print('%s%3d %-6d %5d  %s' % ('*' if i.get('focus') else ' ', i['pos'], i['wgid'], i.get('prio', 0),
                                                                 i.get('name', ''))) for i in r['items']])
    elif cmd == 'fg':
        out(agent('fg', rest[:1]))
    elif cmd in ('unlock', 'lock'):
        out(agent('keylock', ['off' if cmd == 'unlock' else 'on']))
    elif cmd == 'keylock':
        out(agent('keylock', ['query']))
    elif cmd == 'keepawake':
        out(agent('keepawake', rest[:1] or ['on']))
    elif cmd == 'prop':
        out(agent('pget' if rest[0] == 'get' else 'pset', rest[1:4]))
    elif cmd == 'startup':
        # startup add|rm|check PATH [w0 w1 w2] | startup list (reads Starter's start.dat)
        if rest and rest[0] == 'list':
            r = c.call({'op': 'get', 'remote': 'C:\\private\\100059C9\\start.dat'})
            if not r.get('ok'):
                print('(no start.dat: the dynamic startup list is empty)'); return
            import base64 as b64
            for rec in startlist.records(startlist.decrypt(b64.b64decode(r['b64']))):
                print(json.dumps(rec))
            return
        out(agent('startup', rest[:5]))
    elif cmd == 'mem':
        out(agent('mem'))
    elif cmd == 'key':
        ap = argparse.ArgumentParser(); ap.add_argument('keys', nargs='+'); ap.add_argument('--delay', type=float, default=0.3)
        x = ap.parse_args(rest)
        # a pause: a number with a dot, or 2+ digits (single digits are the phone's number keys)
        keys = [float(k) if k.replace('.', '', 1).isdigit() and ('.' in k or len(k) > 1) else k for k in x.keys]
        out(c.call({'op': 'key', 'keys': keys, 'delay': x.delay}))
    elif cmd == 'type':
        out(c.call({'op': 'type', 'text': ' '.join(rest)}))
    elif cmd == 'screenshot':
        ap = argparse.ArgumentParser(); ap.add_argument('out', nargs='?'); ap.add_argument('--scale', type=float, default=1.0)
        ap.add_argument('--changed', action='store_true')
        x = ap.parse_args(rest)
        out(c.call({'op': 'screenshot', 'path': os.path.abspath(x.out) if x.out else None, 'scale': x.scale,
                    'only_if_changed': x.changed}))
    elif cmd == 'log':
        ap = argparse.ArgumentParser(); ap.add_argument('--grep'); ap.add_argument('--since', type=float, default=0)
        ap.add_argument('--max', type=int, default=40); ap.add_argument('--head', action='store_true')
        x = ap.parse_args(rest)
        out(c.call({'op': 'log', 'grep': x.grep, 'since': x.since, 'max': x.max, 'tail': not x.head}),
            lambda r: (print('\n'.join(r['lines'])), r['more'] and print('(%d more)' % r['more'])))
    elif cmd == 'events':
        ap = argparse.ArgumentParser(); ap.add_argument('--since', type=int, default=0)
        ap.add_argument('--wait', type=float, default=0); ap.add_argument('--kind', action='append')
        x = ap.parse_args(rest)
        out(c.call({'op': 'events', 'since': x.since, 'wait': x.wait, 'kinds': x.kind}),
            lambda r: [print(json.dumps(e)) for e in r['events']])
    elif cmd == 'catch':
        sub = rest[0] if rest else 'list'
        if sub == 'add':
            ap = argparse.ArgumentParser(prog='sydb catch add')
            ap.add_argument('--kind', default='panic', choices=['panic', 'kill', 'terminate', 'exit', 'any'])
            ap.add_argument('--thread', help='regex on process::thread'); ap.add_argument('--category')
            ap.add_argument('--reason', type=int); ap.add_argument('--action', default='dump', choices=['dump', 'freeze'])
            ap.add_argument('--snapshot', action='store_true'); ap.add_argument('--once', action='store_true')
            ap.add_argument('--at', help='breakpoint rule: dump every time this address runs (cold code only)')
            ap.add_argument('--max', type=int, default=20, help='breakpoint rule: stop matching after N hits')
            x = vars(ap.parse_args(rest[1:]))
            out(c.call(dict(x, op='catch', sub='add'), timeout=120))
        elif sub == 'rm':
            out(c.call({'op': 'catch', 'sub': 'rm', 'id': int(rest[1]) if len(rest) > 1 else None}, timeout=60))
        elif sub == 'show':
            out(c.call({'op': 'catch', 'sub': 'show', 'id': int(rest[1])}), lambda r: print(r['text'], end=''))
        elif sub in ('list', 'hits', 'resume'):
            out(c.call({'op': 'catch', 'sub': sub}, timeout=60))
        else:
            die('catch add|rm [ID]|list|hits|show ID|resume')
    elif cmd == 'snap':
        sub = rest[0] if rest else 'list'
        if sub == 'save':
            out(c.call({'op': 'snap', 'sub': 'save', 'name': rest[1] if len(rest) > 1 and not rest[1].startswith('-') else None,
                        'stopped': '--stopped' in rest}, timeout=300))
        elif sub == 'rm':
            out(c.call({'op': 'snap', 'sub': 'rm', 'name': rest[1]}))
        else:
            out(c.call({'op': 'snap', 'sub': 'list'}))
    elif cmd == 'gc':
        ap = argparse.ArgumentParser(prog='sydb gc')
        ap.add_argument('--keep-auto', type=int, default=5); ap.add_argument('--keep-dumps', type=int, default=200)
        ap.add_argument('--no-forks', action='store_true'); ap.add_argument('--dry-run', action='store_true')
        x = ap.parse_args(rest)
        out(c.call({'op': 'gc', 'keep_auto': x.keep_auto, 'keep_dumps': x.keep_dumps, 'forks': not x.no_forks,
                    'dry': x.dry_run}, timeout=120))
    elif cmd in ('pause', 'resume'):
        out(c.call({'op': 'vm', 'sub': cmd}))
    elif cmd == 'qmp':
        out(c.call({'op': 'qmp', 'cmd': rest[0], 'args': json.loads(rest[1]) if len(rest) > 1 else {}}),
            lambda r: print(json.dumps(r.get('return'))))
    elif cmd == 'hmp':
        out(c.call({'op': 'hmp', 'cmd': ' '.join(rest)}), lambda r: print(r['out'], end=''))
    elif cmd == 'update-agent':    # deploy a rebuilt agent without stopping the session
        need(c.call({'op': 'put', 'local': AGENT_EXE, 'remote': 'C:\\data\\sydb\\devagent.new', 'stamp': True}))
        need(c.call({'op': 'put', 'local': DAFS_EXE, 'remote': 'C:\\sys\\bin\\dafs.exe', 'stamp': True}))
        need(agent('upgrade', ['C:\\data\\sydb\\devagent.new']))
        time.sleep(1.5)
        r = need(c.call({'op': 'wait', 'what': 'agent', 'timeout': 30}))
        if not r.get('agent'):
            die('agent did not come back')
        # the startup list pins the exe's MD5: register the new binary
        need(agent('startup', ['add', AGENT_PATH]))
        print('agent restarted (pid %s), startup list entry updated' % r['hello'].get('pid'))
    elif cmd == 'strace':
        # strace MATCH [--secs S]: IPC messages to/from processes whose name contains MATCH
        ap = argparse.ArgumentParser(); ap.add_argument('match'); ap.add_argument('--secs', type=float, default=5)
        ap.add_argument('--max', type=int, default=200)
        x = ap.parse_args(rest)
        r = need(c.call({'op': 'trace', 'on': True, 'match': x.match}))
        seq, end, n = r['seq'], time.time() + x.secs, 0
        try:
            while time.time() < end and n < x.max:
                e = c.call({'op': 'events', 'since': seq, 'wait': end - time.time(), 'kinds': ['ipc'], 'max': 500})
                seq = e['last']
                for v in e['events']:
                    n += 1
                    if js:
                        print(json.dumps(v))
                    elif v['kind'] == 'ipc.call':
                        print('%8.3f %s -> %s  %s' % (v['t'], v['client'], v['server'], v.get('op') or 'fn %d' % v['fn']))
                    elif v['kind'] == 'ipc.read':
                        print('%8.3f      %s read "%s"' % (v['t'], v['server'], v['data']))
                    else:
                        print('%8.3f %s <- %s  fn %d = %d' % (v['t'], v['client'], v['server'], v['fn'], v['r']))
        finally:
            c.call({'op': 'trace', 'on': False})
    elif cmd == 'dbg':
        # dbg attach|detach|interrupt|continue [--wait S]|wait [S]|step|next|stepi|finish|regs|bt
        #     break LOC [--thread T] [--once] [--sw]|delete [N]|mem ADDR [LEN]|write ADDR HEX|eval EXPR
        #     symbols NAME [ELF]|where|cmd GDBCMD
        sub, args = rest[0], rest[1:]
        req = {'op': 'dbg', 'sub': sub}
        def opt(name, conv=str, default=None):
            if name in args:
                i = args.index(name); v = conv(args[i + 1]) if conv is not bool else True
                del args[i:i + (1 if conv is bool else 2)]
                return v
            return default
        req['thread'] = opt('--thread')
        w = opt('--wait', float)
        if w is not None:
            req['wait'] = w
        if opt('--once', bool):
            req['once'] = True
        if opt('--sw', bool):
            req['hw'] = False
        if sub == 'break':
            req['loc'] = args[0]
        elif sub == 'delete' and args:
            req['bp'] = args[0]
        elif sub == 'wait' and args:
            req['timeout'] = float(args[0])
        elif sub == 'mem':
            req['addr'] = args[0]; req['len'] = int(args[1], 0) if len(args) > 1 else 64
        elif sub == 'write':
            req['addr'], req['hex'] = args[0], args[1]
        elif sub == 'eval':
            req['expr'] = ' '.join(args)
        elif sub == 'symbols':
            req['name'] = args[0]
            if len(args) > 1:
                req['elf'] = os.path.abspath(args[1])
        elif sub == 'cmd':
            req['cmd'] = ' '.join(args)
        out(c.call(req, timeout=600), (lambda r: print(r['out'])) if sub == 'cmd' else None)
    elif cmd == 'agent':           # raw agent command, for experiments
        out(agent(rest[0], rest[1:]))
    else:
        die('unknown command %s (sydb with no arguments prints help)' % cmd)


if __name__ == '__main__':
    main()
