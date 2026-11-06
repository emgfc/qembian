#!/usr/bin/env python3
"""sydb: adb-like dev/debug bridge for the virtual N95 (see docs/sydb.md).

  sydb start [--seed DIR] [--fresh] [--gui] [--rom ROM]   boot a session (daemon + QEMU), wait for idle
  sydb stop | status | wait idle|agent|REGEX [--timeout S]
  sydb install                       put devagent.exe + its startup-list entry on C: (session stopped)
  sydb ls PATH | push LOCAL REMOTE | pull REMOTE LOCAL | rm PATH | mkdir PATH
  sydb run PATH [ARGS...] [--wait S] | rerun LOCAL.exe [ARGS...] [--wait S] | kill PID|NAME
  sydb ps [PATTERN] | threads [PATTERN] | apps [FILTER] | launch UID|CAPTION | mem
  sydb wg | fg WGID | unlock | lock | keepawake on|off|unlocked
  sydb prop get CAT KEY | prop set CAT KEY VALUE              (hex category/key)
  sydb key KEY... [--delay S] | type TEXT | screenshot [OUT.png] [--scale F] [--changed]
  sydb log [--grep RE] [--since T] [--max N] [--head] | events [--since SEQ] [--wait S] [--kind K]
  sydb update-agent                  push a rebuilt devagent.exe/dafs.exe and restart the agent live
  sydb dbg attach|break LOC [--thread T]|continue [--wait S]|wait|step|next|stepi|finish|regs|bt|
           mem ADDR [LEN]|write ADDR HEX|eval EXPR|symbols NAME [ELF]|interrupt|detach|cmd GDBCMD
  sydb strace MATCH [--secs S]       IPC messages to/from processes matching MATCH (kernel KIPC trace)
  sydb tui                           curses view: status, window groups, events, log, command line
  sydb hmp CMD | mcp (MCP stdio server) | --json (raw JSON output) | --session DIR
Keys: QKeyCode names or aliases lsk rsk ok menu star hash clear back; a number = pause in seconds.
"""
import argparse, json, os, shutil, socket, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
DEFAULT_SESSION = os.path.join(ROOT, 'build', 'sydb')
AGENT_EXE = os.path.join(ROOT, 'build', 'n95apps', 'devagent.exe')
DAFS_EXE = os.path.join(ROOT, 'build', 'n95apps', 'dafs.exe')


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
        subprocess.run([os.path.join(ROOT, 'scripts', 'n95-drive.sh'), 'mkdir', 'c', d], env=env, capture_output=True)
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
    # autostart (hack H-sydb-autostart): EStart runs HALSettings.exe by bare name; dafs.exe under
    # that name spawns devagent.exe and exits with -12 like the N95's own copy
    tmp = os.path.join(sdir, 'HALSettings.exe')
    shutil.copy(DAFS_EXE, tmp)
    drive_put(drives, tmp, '/sys/bin')
    os.unlink(tmp)
    print('installed devagent.exe, dafs.exe (+ HALSettings.exe autostart) on C:')


def cmd_start(sdir, a):
    c = Client(sdir)
    if c.alive():
        print('session already running in %s' % sdir)
        return
    drives = os.path.join(sdir, 'drives')
    os.makedirs(sdir, exist_ok=True)
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
    subprocess.Popen([sys.executable, os.path.abspath(__file__), '--session', sdir, '_daemon', rom, drives]
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
    if not argv:
        print(__doc__); return
    cmd, rest = argv[0], argv[1:]

    if cmd == '_daemon':
        import daemon
        ap = argparse.ArgumentParser(); ap.add_argument('rom'); ap.add_argument('drives')
        ap.add_argument('--gui', action='store_true')
        x = ap.parse_args(rest)
        daemon.serve(sdir, x.rom, x.drives, x.gui, [])
        return
    if cmd == 'tui':
        import tui
        tui.main(sdir)
        return
    if cmd == 'mcp':
        import mcp
        mcp.serve(sdir)
        return
    if cmd in ('start', 'install'):
        ap = argparse.ArgumentParser(prog='sydb ' + cmd)
        ap.add_argument('--seed'); ap.add_argument('--fresh', action='store_true')
        ap.add_argument('--gui', action='store_true'); ap.add_argument('--install', action='store_true')
        ap.add_argument('--no-wait', action='store_true'); ap.add_argument('--timeout', type=float, default=180)
        ap.add_argument('--rom', default=os.path.join(ROOT, 'build', 'n95', 'hybrid.img'))
        a = ap.parse_args(rest)
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
        out(c.call({'op': 'stop'}))
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
    elif cmd == 'mem':
        out(agent('mem'))
    elif cmd == 'key':
        ap = argparse.ArgumentParser(); ap.add_argument('keys', nargs='+'); ap.add_argument('--delay', type=float, default=0.3)
        x = ap.parse_args(rest)
        keys = [float(k) if k.replace('.', '', 1).isdigit() and '.' in k else k for k in x.keys]
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
    elif cmd == 'hmp':
        out(c.call({'op': 'hmp', 'cmd': ' '.join(rest)}), lambda r: print(r['out'], end=''))
    elif cmd == 'update-agent':    # deploy a rebuilt agent without stopping the session
        need(c.call({'op': 'put', 'local': AGENT_EXE, 'remote': 'C:\\data\\sydb\\devagent.new', 'stamp': True}))
        need(c.call({'op': 'put', 'local': DAFS_EXE, 'remote': 'C:\\sys\\bin\\dafs.exe', 'stamp': True}))
        need(agent('upgrade', ['C:\\data\\sydb\\devagent.new']))
        time.sleep(1.5)
        out(c.call({'op': 'wait', 'what': 'agent', 'timeout': 30}),
            lambda r: print('agent restarted (pid %s)' % r['hello'].get('pid') if r.get('agent') else 'agent did not come back'))
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
