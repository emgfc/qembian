"""sydb MCP server (stdio, JSON-RPC 2.0, newline-delimited). Run: tools/sydb/sydb.py mcp

Every tool forwards to the session daemon (tools/sydb/daemon.py) and returns compact JSON text;
ui_screenshot can return the picture inline (MCP image content). Register e.g. in Claude Code:
  claude mcp add sydb -- python3 /path/to/syborg-pg/tools/sydb/sydb.py mcp
"""
import base64, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sydb as S

PROTO = '2024-11-05'


def obj(props, req=()):
    return {'type': 'object', 'properties': props, 'required': list(req)}


STR, NUM, INT, BOOL = {'type': 'string'}, {'type': 'number'}, {'type': 'integer'}, {'type': 'boolean'}
ARR = {'type': 'array', 'items': {'type': 'string'}}

TOOLS = [
    ('session_start', 'Boot the virtual N95 (QEMU + agent) and wait until the idle screen and the agent are up. '
     'seed = drives dir to copy the phone state from (first time); fresh = blank phone (asks region/date).',
     obj({'seed': STR, 'fresh': BOOL, 'gui': BOOL, 'timeout': NUM})),
    ('session_stop', 'Shut the session down (QEMU quits, drives are kept).', obj({})),
    ('session_status', 'State (booting/idle/stopped), uptime, agent info, gdb port.', obj({})),
    ('session_wait', "Wait for 'idle', 'agent', or a regex on the serial log.", obj({'what': STR, 'timeout': NUM}, ['what'])),
    ('log_tail', 'Serial (kernel debug / RDebug) log lines, filtered by regex; t = seconds since boot.',
     obj({'grep': STR, 'since': NUM, 'max': INT, 'head': BOOL})),
    ('events', 'Session events after seq: agent.exit (watched process ended), thread.panic, thread.killed, '
     'phone.idle, agent.hello. wait = seconds to block for new events.', obj({'since': INT, 'kinds': ARR, 'wait': NUM, 'max': INT})),
    ('fs_ls', 'List a directory (or one file) on the phone, e.g. C:\\sys\\bin', obj({'path': STR}, ['path'])),
    ('fs_push', 'Copy a host file to the phone (any path; \\sys\\ via the TCB helper).', obj({'local': STR, 'remote': STR}, ['local', 'remote'])),
    ('fs_pull', 'Copy a phone file to the host.', obj({'remote': STR, 'local': STR}, ['remote', 'local'])),
    ('fs_rm', 'Delete a phone file.', obj({'path': STR}, ['path'])),
    ('app_run', 'Start an EXE on the phone; wait = seconds to wait for its exit (returns exit type/reason/category).',
     obj({'path': STR, 'args': STR, 'wait': NUM}, ['path'])),
    ('app_rerun', 'Kill the running instance (if any), push LOCAL exe to C:\\sys\\bin and run it.',
     obj({'local': STR, 'args': STR, 'wait': NUM}, ['local'])),
    ('app_list', 'Installed applications (uid, caption, exe path); filter = substring.', obj({'filter': STR})),
    ('app_launch', 'Launch an application by hex uid or caption prefix (through apparc, like the menu).',
     obj({'what': STR}, ['what'])),
    ('proc_list', 'Processes (pid, name, exit state); pattern = name prefix.', obj({'pattern': STR})),
    ('thread_list', 'Threads (tid, name, priority, exit state); pattern = name prefix.', obj({'pattern': STR})),
    ('proc_kill', 'Kill a process by hex pid or name prefix.', obj({'target': STR, 'reason': INT}, ['target'])),
    ('ui_windows', 'WSERV window groups front to back (wgid, priority, name status|uid|caption, focus).', obj({})),
    ('ui_foreground', 'Bring a window group to the foreground.', obj({'wgid': INT}, ['wgid'])),
    ('ui_key', 'Press keys: QKeyCode names (up, down, left, right, ret, 0-9, a-z) or aliases lsk rsk ok menu '
     'star hash clear back; chords with "-"; a number in the list = pause seconds.', obj({'keys': {'type': 'array'}, 'delay': NUM}, ['keys'])),
    ('ui_type', 'Type text (US keyboard).', obj({'text': STR}, ['text'])),
    ('ui_screenshot', 'Screenshot (240x320). inline = return the image; only_if_changed = skip when identical to the last one.',
     obj({'scale': NUM, 'only_if_changed': BOOL, 'inline': BOOL, 'path': STR})),
    ('sys_keylock', "Keypad lock: op 'off' (unlock without note), 'on', 'query'.", obj({'op': STR})),
    ('sys_keepawake', "'on' (reset inactivity every 5 s: no screensaver), 'unlocked' (also keep the keypad unlocked), 'off'.",
     obj({'mode': STR}, ['mode'])),
    ('sys_prop', 'Publish&Subscribe integer property get/set (hex category and key).',
     obj({'op': STR, 'category': STR, 'key': STR, 'value': INT}, ['op', 'category', 'key'])),
    ('sys_mem', 'RAM total/free, ROM size.', obj({})),
    ('qemu_hmp', 'QEMU monitor command (e.g. "info registers", "x/8wx 0xc8000000").', obj({'cmd': STR}, ['cmd'])),
]


class Server:
    def __init__(self, sdir):
        self.sdir = sdir
        self.c = S.Client(sdir)

    def call(self, req, timeout=None):
        r = self.c.call(req, timeout)
        if not r.get('ok'):
            raise RuntimeError(r.get('error', 'failed'))
        r.pop('ok', None)
        return r

    def agent(self, cmd, args=(), raw=None, timeout=30):
        r = self.call({'op': 'agent', 'cmd': cmd, 'args': [str(a) for a in args], 'raw': raw, 'timeout': timeout})
        if not r.get('items'):
            r.pop('items', None)
        return r

    def run(self, path, args, wait):
        ev0 = self.call({'op': 'events', 'since': 0})['last']
        r = self.agent('run', [path], raw=args or None)
        res = {'pid': r.get('pid'), 'name': r.get('name')}
        end = time.time() + (wait or 0)
        while wait and time.time() < end:
            e = self.call({'op': 'events', 'since': ev0, 'wait': end - time.time(), 'kinds': ['agent.exit']})
            ev0 = e['last']
            hit = [v for v in e['events'] if v.get('pid') == r.get('pid')]
            if hit:
                res['exit'] = {k: hit[0][k] for k in ('type', 'reason', 'category') if k in hit[0]}
                break
        if wait and 'exit' not in res:
            res['running'] = True
        return res

    def tool(self, name, a):
        if name == 'session_start':
            if self.c.alive():
                return self.call({'op': 'status'})
            cmd = [sys.executable, os.path.join(HERE, 'sydb.py'), '--session', self.sdir, 'start',
                   '--timeout', str(a.get('timeout', 180))]
            if a.get('seed'):
                cmd += ['--seed', a['seed']]
            if a.get('fresh'):
                cmd.append('--fresh')
            if a.get('gui'):
                cmd.append('--gui')
            p = subprocess.run(cmd, capture_output=True, text=True)
            if p.returncode:
                raise RuntimeError((p.stderr or p.stdout).strip()[-400:])
            return {'result': p.stdout.strip()}
        if not self.c.alive():
            raise RuntimeError('no session running: call session_start')
        if name == 'session_stop':
            return self.call({'op': 'stop'})
        if name == 'session_status':
            return self.call({'op': 'status'})
        if name == 'session_wait':
            return self.call({'op': 'wait', 'what': a['what'], 'timeout': a.get('timeout', 60)})
        if name == 'log_tail':
            return self.call({'op': 'log', 'grep': a.get('grep'), 'since': a.get('since', 0), 'max': a.get('max', 30),
                              'tail': not a.get('head')})
        if name == 'events':
            return self.call({'op': 'events', 'since': a.get('since', 0), 'kinds': a.get('kinds'), 'wait': a.get('wait', 0),
                              'max': a.get('max', 30)})
        if name == 'fs_ls':
            return self.agent('ls', [a['path']])
        if name == 'fs_push':
            return self.call({'op': 'put', 'local': os.path.abspath(a['local']), 'remote': a['remote']})
        if name == 'fs_pull':
            return self.call({'op': 'get', 'remote': a['remote'], 'local': os.path.abspath(a['local'])})
        if name == 'fs_rm':
            return self.agent('rm', [a['path']])
        if name == 'app_run':
            return self.run(a['path'], a.get('args'), a.get('wait', 0))
        if name == 'app_rerun':
            exe = os.path.basename(a['local'])
            for it in self.agent('ps', [exe.rsplit('.', 1)[0]]).get('items', []):
                if it.get('exit') == 'pending':
                    self.agent('kill', [it['pid']])
            remote = 'C:\\sys\\bin\\' + exe
            put = self.call({'op': 'put', 'local': os.path.abspath(a['local']), 'remote': remote})
            r = self.run(remote, a.get('args'), a.get('wait', 0))
            r['pushed'] = put.get('size')
            return r
        if name == 'app_list':
            r = self.agent('apps', timeout=60)
            f = (a.get('filter') or '').lower()
            return {'apps': [i for i in r.get('items', []) if f in (str(i.get('caption', '')) + str(i.get('path', ''))).lower()]}
        if name == 'app_launch':
            return self.agent('launch', [a['what']], timeout=60)
        if name == 'proc_list':
            return {'processes': self.agent('ps', [a['pattern']] if a.get('pattern') else []).get('items', [])}
        if name == 'thread_list':
            return {'threads': self.agent('threads', [a['pattern']] if a.get('pattern') else []).get('items', [])}
        if name == 'proc_kill':
            return self.agent('kill', [a['target']] + ([a['reason']] if 'reason' in a else []))
        if name == 'ui_windows':
            r = self.agent('wg')
            r['groups'] = [i for i in r.pop('items', []) if i.get('name')]   # unnamed groups are noise
            return r
        if name == 'ui_foreground':
            return self.agent('fg', [a['wgid']])
        if name == 'ui_key':
            return self.call({'op': 'key', 'keys': a['keys'], 'delay': a.get('delay', 0.3)})
        if name == 'ui_type':
            return self.call({'op': 'type', 'text': a['text']})
        if name == 'ui_screenshot':
            r = self.call({'op': 'screenshot', 'path': a.get('path'), 'scale': a.get('scale', 1.0),
                           'only_if_changed': a.get('only_if_changed', False)})
            if a.get('inline', True) and r.get('path'):
                return {'_image': base64.b64encode(open(r['path'], 'rb').read()).decode(), **r}
            return r
        if name == 'sys_keylock':
            return self.agent('keylock', [a.get('op', 'query')])
        if name == 'sys_keepawake':
            return self.agent('keepawake', [a['mode']])
        if name == 'sys_prop':
            if a['op'] == 'set':
                return self.agent('pset', [a['category'], a['key'], a.get('value', 0)])
            return self.agent('pget', [a['category'], a['key']])
        if name == 'sys_mem':
            return self.agent('mem')
        if name == 'qemu_hmp':
            return self.call({'op': 'hmp', 'cmd': a['cmd']})
        raise RuntimeError('unknown tool ' + name)

    def handle(self, msg):
        m, mid = msg.get('method'), msg.get('id')
        if m == 'initialize':
            return {'protocolVersion': PROTO, 'capabilities': {'tools': {}},
                    'serverInfo': {'name': 'sydb', 'version': '0.1'}}
        if m == 'tools/list':
            return {'tools': [{'name': n, 'description': d, 'inputSchema': s} for n, d, s in TOOLS]}
        if m == 'tools/call':
            p = msg.get('params', {})
            try:
                r = self.tool(p['name'], p.get('arguments') or {})
                content = []
                img = r.pop('_image', None) if isinstance(r, dict) else None
                content.append({'type': 'text', 'text': json.dumps(r, separators=(',', ':'))})
                if img:
                    content.append({'type': 'image', 'data': img, 'mimeType': 'image/png'})
                return {'content': content}
            except Exception as e:
                return {'content': [{'type': 'text', 'text': 'error: %s' % e}], 'isError': True}
        if m == 'ping':
            return {}
        if mid is None:
            return None            # notification
        raise KeyError(m)


def serve(sdir):
    srv = Server(sdir)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        mid = msg.get('id')
        try:
            res = srv.handle(msg)
            if mid is None:
                continue
            out = {'jsonrpc': '2.0', 'id': mid, 'result': res}
        except KeyError as e:
            out = {'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32601, 'message': 'method not found: %s' % e}}
        except Exception as e:
            out = {'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32603, 'message': str(e)}}
        sys.stdout.write(json.dumps(out) + '\n')
        sys.stdout.flush()
