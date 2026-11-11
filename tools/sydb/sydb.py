#!/usr/bin/env python3
"""sydb: qembian's adb-like dev/debug bridge to the virtual phone (see docs/sydb.md).

Which phone: --instance NAME (a device instance, tools/instance.py: build/instances/NAME), --session DIR
(any session directory; an instance's directory is one), else the primary instance of $SYBORG_DEVICE
(default: models/registry.toml `default`), made at its first start (build/instances/<device id>).

  sydb phone [NAME] [--fresh] [--scale 2]   emulator window of instance NAME: screen, phone keys (long
                                     presses), reset/pause/screenshot/snapshot, slider; starts the
                                     session if none runs and ends it when the window closes
  sydb start [--seed DIR] [--fresh] [--gui] [--rom ROM] [--audio wav|host|DRIVER] [--paused | --catch-panics]
                                              boot a session, wait for idle
  sydb stop | status | wait idle|agent|REGEX [--timeout S]
  sydb install | uninstall           agent on/off C: incl. its Starter startup-list record (session stopped)
  sydb ls PATH | push LOCAL REMOTE | pull REMOTE LOCAL | rm PATH | mkdir PATH
  sydb run PATH [ARGS...] [--wait S] | rerun LOCAL.exe [ARGS...] [--wait S] | kill PID|NAME
  sydb sis install ITEM... [--drive E] [--lang N] [--keep] [--timeout S]
                                     install without the installer UI: every installer question is
                                     answered yes and logged (<session>/sis/NAME.log, serial SISINST:);
                                     ITEM = local file/dir (copied over the agent link) or phone path
                                     E:\\x.sisx / E:\\dir\\ (installed in place)
  sydb drive ls|put|get|mkdir|rm LETTER ...   files in the session's drive images, session stopped
                                     (fast: e.g. drive put E ~/games/ installs, then sis install E:\\installs\\)
  sydb ps [PATTERN] | threads [PATTERN] | apps [FILTER] | launch UID|CAPTION | mem
  sydb startup add|rm|check PATH [w0 w1 w2] | startup list   (Starter's dynamic startup list)
  sydb wg | fg WGID | unlock | lock | keepawake on|off|unlocked
  sydb prop get CAT KEY | prop set CAT KEY VALUE              (hex category/key)
  sydb key KEY... [--delay S] | type TEXT | screenshot [OUT.png] [--scale F] [--changed]
  sydb gl [get] | gl set NAME=V... [--save]   host GPU settings (msaa, filter, compose), stats
  sydb accel [portrait|landscape|flat|X,Y,Z]  the gravity vector (milli-g) the accelerometer reports; no arg: read
  sydb log [--grep RE] [--since T] [--max N] [--head] | events [--since SEQ] [--wait S] [--kind K]
  sydb update-agent                  push a rebuilt devagent.exe/dafs.exe and restart the agent live
  sydb dbg attach|break LOC [--thread T]|continue [--wait S]|wait|step|next|stepi|finish|regs|bt|
           mem ADDR [LEN]|write ADDR HEX|eval EXPR|symbols NAME [ELF]|interrupt|detach|cmd GDBCMD
  sydb strace MATCH [--secs S]       IPC messages to/from processes matching MATCH (kernel KIPC trace)
  sydb tui                           curses view: status, window groups, events, log, command line
  sydb catch add --at ADDR [--thread RE] [--max N] [--action dump|freeze]   dump each time ADDR runs
  sydb catch add [--kind panic|kill|terminate|exit|any] [--thread RE] [--category CAT] [--reason N]
                 [--action dump|freeze] [--snapshot] [--once]   catch thread deaths (see docs/sydb.md)
  sydb catch list | hits | show ID | rm [ID] | resume
  sydb snap save [NAME] [--stopped] | snap list | snap rm NAME     machine snapshots (RAM+devices+drives)
  sydb fork SNAP [--into DIR] [--run] [--gui]   resume a snapshot in a new session (paused by default)
  sydb restore SNAP [--run]          the same into this session  | sydb pause | resume
  sydb gc [--keep-auto 5] [--keep-dumps 200] [--no-forks] [--dry-run]   retention (also after each catch hit)
  sydb qmp CMD [JSON-ARGS] | hmp CMD | mcp (MCP stdio server) | --json (raw JSON output)
  --instance NAME | --session DIR   (before or after the command)
Keys: QKeyCode names or aliases lsk rsk ok menu star hash clear back call end power volup voldown; 1.5 or 10 = pause in seconds (1 = key 1);
      KEY:SECONDS = hold (long press), e.g. menu:1.5 opens the task switcher.
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, tempfile, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel  # tools/device.py: build paths of the device ($SYBORG_DEVICE / $SYBORG_INSTANCE)
import instance as instmod  # tools/instance.py: device instances


def _pick_instance(argv):
    """the instance this command runs on: --instance NAME, `phone NAME`, or a --session DIR that is an
    instance. Its device and [overrides] then hold for this process and every tool it starts
    (SYBORG_DEVICE, SYBORG_INSTANCE); --instance becomes --session DIR. None: no instance named."""
    name = None
    if '--instance' in argv:
        i = argv.index('--instance')
        if i + 1 >= len(argv):
            sys.exit('sydb: --instance NAME')
        name = argv[i + 1]
        del argv[i:i + 2]
    elif '--session' in argv:
        i = argv.index('--session')
        d = os.path.abspath(argv[i + 1]) if i + 1 < len(argv) else ''
        if os.path.isfile(os.path.join(d, 'instance.toml')):
            name = d
    else:
        rest = [x for x in argv if x != '--json']
        if len(rest) > 1 and rest[0] == 'phone' and not rest[1].startswith('-'):
            name = rest[1]
            argv.remove(name)
    if name is None:
        return None
    try:
        inst = instmod.read(name)
        dev = instmod.device_id(inst)
    except (instmod.InstanceError, devmodel.DeviceError) as e:
        sys.exit('sydb: %s' % e)
    # the phone named on the command line wins over a SYBORG_DEVICE left in the shell (it only picks the
    # primary phone of a device when no phone is named)
    if os.environ.get('SYBORG_DEVICE') and devmodel.resolve(os.environ['SYBORG_DEVICE']) != dev:
        sys.stderr.write('sydb: phone %s is %s; SYBORG_DEVICE=%s ignored\n' % (inst['name'], dev, os.environ['SYBORG_DEVICE']))
    os.environ['SYBORG_DEVICE'] = dev
    os.environ['SYBORG_INSTANCE'] = inst['_dir']
    if '--session' not in argv:
        argv[:0] = ['--session', inst['_dir']]
    return inst


_ARGV = sys.argv[1:]
INSTANCE = _pick_instance(_ARGV)
sys.argv[1:] = _ARGV
DEVPATHS = devmodel.paths(devmodel.load())
sys.path.insert(0, HERE)
DEVICE_ID = devmodel.load()['_id']
# no instance named: the device's primary instance (its drives are that phone's), made at the first start
# as build/instances/<device id>
_PRIMARY = None if INSTANCE else instmod.primary_for(DEVICE_ID)
DEFAULT_SESSION = _PRIMARY['_dir'] if _PRIMARY else instmod.idir(DEVICE_ID.replace('@', '_'))
if _PRIMARY and '--session' not in sys.argv:
    os.environ['SYBORG_INSTANCE'] = _PRIMARY['_dir']	# its [overrides] for the daemon and the tools it runs
    INSTANCE = _PRIMARY
AGENT_EXE = os.path.join(ROOT, 'build', 'n95apps', 'devagent.exe')
DAFS_EXE = os.path.join(ROOT, 'build', 'n95apps', 'dafs.exe')
SISINST_EXE = os.path.join(ROOT, 'build', 'n95apps', 'sisinst.exe')  # `sis install` (src/n95apps/devagent/sisinst.cpp)
SIS_ERRORS = {   # Software Install results (appinstall swi/inc/swi/sisinstallerrors.h) and the usual system ones
    0: 'installed', -1: 'not found', -3: 'cancelled (a dialog said no, or the installer gave up)',
    -5: 'not supported', -11: 'already exists', -14: 'in use', -20: 'corrupt', -21: 'access denied',
    -26: 'disk full', -46: 'permission denied (capabilities / signing)', -10147: 'would overwrite a file',
    -10149: 'not enough space to install', -10200: 'installer leave', -10250: 'policy file corrupt',
    -10251: 'signature scheme not supported', -10252: 'digest not supported', -10253: 'bad hash',
    -10254: 'security error (signature / certificate)', -10263: 'bad usage'}
HOOK_EXE = os.path.join(ROOT, 'build', 'n95apps', 'boothook.exe')  # the boot hook (docs/agent-autostart.md)


def hook_name():
    """the program of this firmware's EStart the boot hook stands in for (device.py boot_hook_name)"""
    try:
        n = devmodel.boot_hook_name(devmodel.load())
    except OSError:
        n = None
    if not n:
        die('no boot hook name for %s: its EStart starts none of %s (docs/agent-autostart.md)'
            % (DEVICE_ID, ', '.join(devmodel.BOOT_HOOK_NAMES)))
    return n
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
        subprocess.run([os.path.join(ROOT, 'scripts', 'drive.sh'), 'mkdir', 'c', d], env=env, capture_output=True, stdin=subprocess.DEVNULL)
    tmp = None
    if name and os.path.basename(local) != name:        # copy under the final name: mtools' mren asserts on
        tmp = tempfile.mkdtemp(prefix='sydb-put-')       # some firmware FAT images (N97 UDA)
        shutil.copy(local, os.path.join(tmp, name))
        local = os.path.join(tmp, name)
    args = [os.path.join(ROOT, 'scripts', 'drive.sh'), 'put', 'c', local, remote_dir]
    r = subprocess.run(args, env=env, capture_output=True, text=True)
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode:
        die('drive.sh failed: ' + (r.stderr or r.stdout)[-500:])


def cmd_install(sdir, a):
    drives = os.path.join(sdir, 'drives')
    if Client(sdir).alive():
        die('stop the session first (the C: image is in use)')
    if not os.path.exists(os.path.join(drives, 'nvmemory.img')):
        die('no drives in %s yet: run "sydb start --seed DIR" (or --fresh) once' % drives)
    for f in (AGENT_EXE, DAFS_EXE, HOOK_EXE):
        if not os.path.exists(f):
            die('%s missing: run src/n95apps/devagent/build.sh' % f)
    drive_put(drives, AGENT_EXE, '/sys/bin')
    drive_put(drives, DAFS_EXE, '/sys/bin')
    # autostart, one mechanism on every OS generation (docs/agent-autostart.md): EStart starts
    # HALSettings.exe (9.2+) or LanguageSelection.exe (9.1) by bare name and the loader searches C: before
    # Z:, so the hook installed under that name starts the agent and then the real z: program
    hook = hook_name()
    for n in devmodel.BOOT_HOOK_NAMES:
        if n != hook:
            drive_rm(drives, '/sys/bin/' + n)
    drive_put(drives, HOOK_EXE, '/sys/bin', name=hook)
    data = drive_read(drives, '/' + startlist.PATH)      # an older install's Starter record: the agent
    if data:                                             # would start twice
        new = startlist.remove(data, AGENT_PATH)
        if new:
            drive_write(drives, '/' + startlist.PATH, new)
        else:
            drive_rm(drives, '/' + startlist.PATH)
    print('installed devagent.exe + dafs.exe on C: and the boot hook c:\\sys\\bin\\%s' % hook)


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
    for f in ('devagent.exe', 'dafs.exe') + devmodel.BOOT_HOOK_NAMES:
        drive_rm(drives, '/sys/bin/' + f)
    print('removed devagent.exe, dafs.exe and the boot hook from C:')


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


def session_instance(sdir):
    """the instance a session directory is (None: a plain session)"""
    try:
        return instmod.read(os.path.abspath(sdir)) if os.path.isfile(os.path.join(sdir, 'instance.toml')) else None
    except instmod.InstanceError:
        return None


def setting(key, default, sdir=None):
    """an instance setting (settings.audio, gl, scale) of the session's instance, else default"""
    inst = session_instance(sdir) if sdir else INSTANCE
    return (inst or {}).get('settings', {}).get(key, default) if inst else default


def session_rom(sdir):
    """the ROM a session boots by default: its instance's (rom_variant), else the device's hybrid.img"""
    inst = session_instance(sdir)
    if inst:
        try:
            return instmod.rom(inst)
        except (instmod.InstanceError, devmodel.DeviceError) as e:
            die(str(e))
    return DEVPATHS['rom']


def cmd_drive(sdir, rest):
    """sydb drive ls|put|get|mkdir|rm LETTER ...: files in the session's drive images, with the session
    stopped (two writers corrupt the FAT). Disk to disk, so large packages go in at once instead of over
    the agent's serial link. put takes files and directories (their files) and creates the target dir."""
    ap = argparse.ArgumentParser(prog='sydb drive')
    ap.add_argument('sub', choices=['ls', 'put', 'get', 'mkdir', 'rm'])
    ap.add_argument('letter'); ap.add_argument('args', nargs='*')
    x = ap.parse_args(rest)
    imgs = {d['letter'].upper(): d['image'] for d in devmodel.get(devmodel.load(), 'board.drives', [])
            if d.get('image')}
    L = x.letter.rstrip(':').upper()
    if L not in imgs:
        die('%s has no drive image for %s: (drives with images: %s)' % (DEVICE_ID, L, ', '.join(sorted(imgs))))
    img = os.path.join(sdir, 'drives', imgs[L])
    if not os.path.exists(img):
        die('%s does not exist: boot the session once (the phone formats its drives)' % img)
    if Client(sdir).alive():
        die('the session is running: sydb stop first (two writers corrupt the FAT)')
    drive = [os.path.join(ROOT, 'scripts', 'drive.sh')]
    env = dict(os.environ, DRIVE_E=img)
    norm = lambda p: p.replace('\\', '/').split(':', 1)[-1] if re.match(r'^[A-Za-z]:', p) else p.replace('\\', '/')
    if x.sub == 'put':
        if not x.args:
            die('put needs LOCAL... [DIR]')
        files, dst = x.args, '/'
        if len(files) > 1 and not os.path.exists(files[-1]):
            files, dst = files[:-1], norm(files[-1])
        exp = []
        for f in files:
            exp += [os.path.join(f, n) for n in sorted(os.listdir(f)) if os.path.isfile(os.path.join(f, n))] \
                if os.path.isdir(f) else [f]
        d = ''
        for part in dst.strip('/').split('/'):
            if part:
                d += '/' + part
                subprocess.run(drive + ['mkdir', 'e', d], env=env, capture_output=True, stdin=subprocess.DEVNULL)
        sys.exit(subprocess.run(drive + ['put', 'e'] + exp + [dst], env=env).returncode)
    sys.exit(subprocess.run(drive + [x.sub, 'e'] + [norm(a) if i == 0 else a for i, a in enumerate(x.args)],
                            env=env).returncode)


def cmd_start(sdir, a):
    c = Client(sdir)
    if c.alive():
        print('session already running in %s' % sdir)
        return
    drives = os.path.join(sdir, 'drives')
    os.makedirs(sdir, exist_ok=True)
    if os.path.abspath(sdir) == os.path.abspath(DEFAULT_SESSION) and not os.path.isfile(os.path.join(sdir, 'instance.toml')):
        # the device's first phone: its primary instance (tools/instance.py)
        inst = instmod.new(os.path.basename(sdir), DEVICE_ID, primary=True)
        instmod.write(inst)
        os.environ['SYBORG_INSTANCE'] = inst['_dir']
        print('new instance %s (%s): tools/instance.py rename/clone/set' % (inst['name'], DEVICE_ID))
    if a.fresh and os.path.isdir(drives):
        shutil.rmtree(drives)
    # a session's drives belong to one phone: never boot another phone's C:/E: (the session records
    # its device)
    mark = os.path.join(sdir, 'device')
    has = os.path.isdir(drives) and bool(os.listdir(drives))
    owner_dev = open(mark).read().strip() if os.path.exists(mark) else (devmodel.default_device() if has else None)
    if owner_dev and owner_dev != DEVICE_ID and not a.seed and has:
        die('session %s holds %s drives, not %s: use --fresh (wipes them) or another --session' % (sdir, owner_dev, DEVICE_ID))
    open(mark, 'w').write(DEVICE_ID + '\n')
    if a.seed:
        if os.path.isdir(drives):
            shutil.rmtree(drives)
        shutil.copytree(a.seed, drives)
    os.makedirs(drives, exist_ok=True)
    if getattr(a, 'no_uda', False):
        os.environ['SYBORG_UDA'] = '0'          # also for the drives run-modern.sh creates
    prof = devmodel.load()
    for kind, name in (('nvmem', 'nvmemory.img'), ('nvmem2', 'nvmemory2.img')):
        # a drive with a user data layer is made now (blank ones by run-modern.sh at the first boot), so
        # the agent can go onto a factory C: before the first boot
        d = [x for x in devmodel.get(prof, 'board.drives', []) if x.get('kind') == kind]
        src = d and devmodel.uda_image(prof, d[0]['letter'])
        if src and os.path.exists(src) and not os.path.exists(os.path.join(drives, name)) \
                and os.environ.get('SYBORG_UDA', '1') != '0':
            print(devmodel.drive_image(prof, kind, os.path.join(drives, name)))
    if a.seed or a.fresh or a.install:
        if os.path.exists(os.path.join(drives, 'nvmemory.img')):
            cmd_install(sdir, a)
    rom = os.path.abspath(a.rom)
    log = open(os.path.join(sdir, 'daemon.log'), 'w')
    subprocess.Popen([sys.executable, os.path.abspath(__file__), '--session', sdir, '_daemon', rom, drives,
                      '--audio', getattr(a, 'audio', None) or 'wav', '--gl', getattr(a, 'gl', None) or 'host']
                     + (['--gui'] if a.gui else [])
                     + (['--paused'] if getattr(a, 'paused', False) or getattr(a, 'catch_panics', False) else [])
                     + (['--owner', str(a.owner)] if getattr(a, 'owner', None) else []),
                     stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                     start_new_session=True, cwd=ROOT)
    for _ in range(100):
        time.sleep(0.1)
        if c.alive():
            break
    else:
        die('daemon did not come up, see %s' % os.path.join(sdir, 'daemon.log'))
    if getattr(a, 'catch_panics', False) and not getattr(a, 'paused', False):
        # the first-boot method (docs/adding-a-device.md): every panic from the first instruction on
        # freezes the machine with a dump and a snapshot; sydb catch hits / catch show ID read them
        need(c.call({'op': 'catch', 'sub': 'add', 'kind': 'panic', 'action': 'freeze', 'snapshot': True,
                     'once': True}, timeout=120))
        need(c.call({'op': 'vm', 'sub': 'resume'}))
    if a.no_wait or getattr(a, 'paused', False):
        print('started%s; session %s' % (' paused before the first instruction (arm catches/breakpoints, then '
                                          'sydb resume)' if getattr(a, 'paused', False) else '', sdir))
        return
    r = c.call({'op': 'wait', 'what': 'idle', 'timeout': a.timeout})
    if getattr(a, 'catch_panics', False):
        hits = c.call({'op': 'catch', 'sub': 'hits'}).get('hits', [])
        for h in hits:
            print('caught %s %s %s %s at %.1f s (frozen there; sydb catch resume goes on): %s' % (
                h['thread'], h['kind'], h['category'], h['reason'], h['t'], h['files']['txt']))
        if hits:
            return
    need(r)
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
    if not argv or argv[0] in ('-h', '--help', 'help'):
        print(__doc__); return
    cmd, rest = argv[0], argv[1:]

    if cmd == '_daemon':
        import daemon
        ap = argparse.ArgumentParser(); ap.add_argument('rom'); ap.add_argument('drives')
        ap.add_argument('--gui', action='store_true'); ap.add_argument('--restore')
        ap.add_argument('--audio', default='wav'); ap.add_argument('--gl', default='host')
        ap.add_argument('--paused', action='store_true')
        ap.add_argument('--owner', type=int, help='stop the session when this process is gone (sydb phone)')
        x = ap.parse_args(rest)
        daemon.serve(sdir, x.rom, x.drives, x.gui, ['-S'] if x.paused else [], x.restore, x.audio, x.gl, x.owner)
        return
    if cmd == 'phone':                 # emulator window: screen + phone keys + machine controls
        ap = argparse.ArgumentParser(prog='sydb phone')
        ap.add_argument('--scale', type=float, help="screen scale (default: the instance's settings.scale, else 1)")
        ap.add_argument('--audio', help="sound of a session it starts: host (speakers), wav, or a QEMU audiodev driver "
                        "(default: the instance's settings.audio, else host)")
        ap.add_argument('--rom', help="ROM of a session it starts (default: the instance's, tools/instance.py get NAME rom)")
        ap.add_argument('--install', action='store_true', help='install/update the sydb agent on its C: first')
        ap.add_argument('--keep', action='store_true', help='leave the session running when the window closes')
        ap.add_argument('--fresh', action='store_true', help='start from empty drives (a factory-fresh phone); restarts a running session')
        ap.add_argument('--no-uda', action='store_true', help="new drives start blank, without the firmware's user data layer")
        ap.add_argument('--restart', action='store_true', help='restart a running session first (fresh boot, current ROM)')
        ap.add_argument('--gl', choices=['host', 'soft'], help="GLES on the host GPU or the software rasteriser (default: the instance's settings.gl, else host)")
        x = ap.parse_args(rest)
        x.audio = x.audio or setting('audio', 'host')
        x.gl = x.gl or setting('gl', 'host')
        x.scale = x.scale or setting('scale', 1.0)
        c0 = Client(sdir)
        if c0.alive():
            st = c0.call({'op': 'status'})
            rom = st.get('rom')
            stale = rom and os.path.exists(rom) and os.path.getmtime(rom) > (st.get('rom_mtime') or 0) + 1
            if x.restart or stale or x.fresh:
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
                            '--audio', x.audio, '--gl', x.gl] + (['--rom', x.rom] if x.rom else [])
                           + (['--install'] if x.install else []) + (['--fresh'] if x.fresh else [])
                           + (['--no-uda'] if x.no_uda else [])
                           + ([] if x.keep else ['--owner', str(os.getpid())]), check=True)
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
    if cmd == 'drive':
        cmd_drive(sdir, rest)
        return
    if cmd in ('start', 'install'):
        ap = argparse.ArgumentParser(prog='sydb ' + cmd)
        ap.add_argument('--seed'); ap.add_argument('--fresh', action='store_true')
        ap.add_argument('--gui', action='store_true'); ap.add_argument('--install', action='store_true')
        ap.add_argument('--no-uda', action='store_true', help="new drives start blank, without the firmware's "
                        'user data layer (default: a copy of it where the profile declares one)')
        ap.add_argument('--no-wait', action='store_true'); ap.add_argument('--timeout', type=float, default=180)
        ap.add_argument('--gl', choices=['host', 'soft'], help="GLES on the host GPU (syborg-gl) or the software rasteriser "
                        "(default: the instance's settings.gl, else host)")
        ap.add_argument('--audio', help="wav (<session>/audio.wav), host (speakers: $AUDIO or pipewire) or any QEMU "
                        "audiodev driver (pa, alsa, sdl, none); default: the instance's settings.audio, else wav")
        ap.add_argument('--rom', help="default: the instance's ROM (its rom_variant), else the device's hybrid.img")
        ap.add_argument('--paused', action='store_true', help='QEMU starts stopped (-S): arm catches or '
                        'breakpoints for early boot, then sydb resume')
        ap.add_argument('--catch-panics', action='store_true', help='start stopped, arm a freeze+snapshot catch '
                        'on the first panic, resume, wait for idle; on a timeout print what was caught')
        ap.add_argument('--owner', type=int, help=argparse.SUPPRESS)	# sydb phone: the session ends with it
        a = ap.parse_args(rest)
        a.rom = a.rom or session_rom(sdir)
        a.audio = a.audio or setting('audio', 'wav', sdir)
        a.gl = a.gl or setting('gl', 'host', sdir)
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
    elif cmd == 'sis':
        ap = argparse.ArgumentParser(prog='sydb sis')
        ap.add_argument('sub', choices=['install'])
        ap.add_argument('items', nargs='+', help='local .sis/.sisx files or directories (copied over the agent '
                        'link), or phone paths: E:\\x.sisx, or E:\\dir\\ for every package in it (no copy)')
        ap.add_argument('--drive', default='E', help='target drive the installer is told to pick (else most space)')
        ap.add_argument('--lang', type=int, default=-1, help='language code to pick (default English, else the first)')
        ap.add_argument('--keep', action='store_true', help='leave copied packages on the phone')
        ap.add_argument('--timeout', type=float, default=900, help='per package')
        x = ap.parse_args(rest)
        if not os.path.exists(SISINST_EXE):
            die('%s missing: src/n95apps/devagent/build.sh' % SISINST_EXE)
        stage = None
        for d in ('E:\\sydb\\', 'C:\\data\\sydb\\'):         # the mass memory if there is one
            agent('mkdir', [d])
            if agent('ls', [d]).get('ok'):
                stage = d
                break
        if not stage:
            die('no staging directory on E: or C:')
        is_sis = lambda n: n.lower().endswith(('.sis', '.sisx'))
        jobs = []                                       # (phone path, local path to copy or None)
        for it in x.items:
            if re.match(r'^[A-Za-z]:\\', it):
                if it.endswith('\\'):
                    r = need(agent('ls', [it]))
                    jobs += [(it + i['name'], None) for i in sorted(r['items'], key=lambda i: i['name'].lower())
                             if i.get('type') != 'dir' and is_sis(i['name'])]
                else:
                    jobs.append((it, None))
            elif os.path.isdir(it):
                jobs += [(None, os.path.join(it, n)) for n in sorted(os.listdir(it)) if is_sis(n)]
            elif os.path.isfile(it):
                jobs.append((None, it))
            else:
                die('%s: no such file (phone paths start with a drive letter: E:\\...)' % it)
        if not jobs:
            die('no .sis/.sisx packages found')
        need(c.call({'op': 'put', 'local': SISINST_EXE, 'remote': 'C:\\sys\\bin\\sisinst.exe', 'stamp': True}))
        os.makedirs(os.path.join(sdir, 'sis'), exist_ok=True)
        results = []
        for remote, local in jobs:
            name = re.sub(r'[^A-Za-z0-9._-]', '_', os.path.basename(local) if local else remote.rsplit('\\', 1)[-1])
            print('== %s' % (local or remote))
            if local:
                remote = stage + name
                t0 = time.time()
                need(c.call({'op': 'put', 'local': os.path.abspath(local), 'remote': remote}))
                print('pushed %d bytes in %.1f s -> %s' % (os.path.getsize(local), time.time() - t0, remote))
            log = stage + name + '.log'
            ev0 = c.call({'op': 'events', 'since': 0})['last']
            r = need(agent('run', ['C:\\sys\\bin\\sisinst.exe'],
                           raw='%s drive=%s lang=%d log=%s' % (remote, x.drive[:1].upper(), x.lang, log)))
            end, ex = time.time() + x.timeout, None
            while ex is None and time.time() < end:
                e = c.call({'op': 'events', 'since': ev0, 'wait': min(5, end - time.time()), 'kinds': ['agent.exit']})
                ev0 = e['last']
                hit = [v for v in e['events'] if v.get('pid') == r.get('pid')]
                if hit:
                    ex = hit[0]
            local_log = os.path.join(sdir, 'sis', name + '.log')
            g = c.call({'op': 'get', 'remote': log, 'local': local_log})
            if g.get('ok'):
                sys.stdout.write(open(local_log, errors='replace').read())
            else:
                print('(no log: %s; the serial log has the SISINST: lines)' % g.get('error'))
            if ex is None:
                print('sisinst still running after %d s (pid %s): see %s/serial.log (SISINST:); stopping here'
                      % (x.timeout, r.get('pid'), sdir))
                results.append((name, 'timeout'))
                break
            if ex is not None:
                agent('rm', [log])
                if local and not x.keep:
                    agent('rm', [remote])
            if ex.get('type') != 'kill':
                res = 'died: %s %s %s' % (ex.get('type'), ex.get('category', ''), ex.get('reason'))
            else:
                code = ex.get('reason', 0)
                res = '%d %s' % (code, SIS_ERRORS.get(code, '(see sisinstallerrors.h / system errors)'))
            print('result %s; log %s' % (res, local_log))
            results.append((name, res))
        if len(results) > 1:
            print('== summary')
            for n, res in results:
                print('  %-40s %s' % (n, res))
        if any(not res.startswith('0 ') for _, res in results):
            sys.exit(1)
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
    elif cmd == 'gl':                     # host GPU settings and statistics (QOM /machine/gl)
        def qom_get(prop):
            r = c.call({'op': 'qmp', 'cmd': 'qom-get', 'args': {'path': '/machine/gl', 'property': prop}})
            if 'error' in r:
                raise SystemExit('sydb gl: %s' % r['error'])
            return r.get('return')
        if not rest or rest[0] in ('get', 'stats'):
            vals = {k: qom_get(k) for k in ('msaa', 'filter', 'compose')}
            st = json.loads(qom_get('stats'))
            if js:
                print(json.dumps({'settings': vals, 'stats': st}))
            else:
                print('settings: ' + ', '.join('%s=%s' % kv for kv in vals.items()))
                print('stats:    ' + ', '.join('%s=%s' % kv for kv in st.items()))
        elif rest[0] == 'set':
            save = '--save' in rest
            sets = [a for a in rest[1:] if a != '--save']
            if not sets:
                raise SystemExit('sydb gl set NAME=VALUE... [--save]   (msaa=0|2|4|8|16 filter=auto|nearest|linear compose=host|guest)')
            changed = {}
            for a in sets:
                k, _, v = a.partition('=')
                val = int(v) if k == 'msaa' and v.isdigit() else v
                r = c.call({'op': 'qmp', 'cmd': 'qom-set', 'args': {'path': '/machine/gl', 'property': k, 'value': val}})
                if 'error' in r:
                    raise SystemExit('sydb gl set %s: %s' % (a, r['error']))
                changed[k] = val
            if save:                       # applied as -global at the next start of this session
                path = os.path.join(sdir, 'gl.json')
                try:
                    cur = json.load(open(path))
                except (OSError, ValueError):
                    cur = {}
                cur.update(changed)
                json.dump(cur, open(path, 'w'), indent=1)
            print('set ' + ', '.join('%s=%s' % kv for kv in changed.items()) + (' (saved)' if save else ''))
        else:
            raise SystemExit('sydb gl [get] | sydb gl set NAME=VALUE... [--save]')
    elif cmd == 'accel':                  # the phone's gravity vector (QEMU syborg-sensor; accelerometer stand-in)
        named = {'portrait': '0,1000,0', 'landscape': '1000,0,0', 'flat': '0,0,1000'}
        if rest:
            v = named.get(rest[0], rest[0])
            need(c.call({'op': 'qmp', 'cmd': 'qom-set', 'args': {'path': '/machine/sensor', 'property': 'accel', 'value': v}}))
        r = need(c.call({'op': 'qmp', 'cmd': 'qom-get', 'args': {'path': '/machine/sensor', 'property': 'accel'}}))
        print(r.get('return'))
    elif cmd == 'tap':                    # touch phones: press at screen pixel (x, y), hold, release
        ap = argparse.ArgumentParser(prog='sydb tap')
        ap.add_argument('x', type=int); ap.add_argument('y', type=int)
        ap.add_argument('--ms', type=int, default=120, help='how long the finger stays down')
        x = ap.parse_args(rest)
        disp = devmodel.get(devmodel.load(), 'board.display', {})
        w, h = disp.get('width', 240), disp.get('height', 320)
        ax, ay = int(x.x * 0x7fff / max(w - 1, 1)), int(x.y * 0x7fff / max(h - 1, 1))
        pos = [{'type': 'abs', 'data': {'axis': 'x', 'value': ax}}, {'type': 'abs', 'data': {'axis': 'y', 'value': ay}}]
        need(c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': pos + [
            {'type': 'btn', 'data': {'down': True, 'button': 'left'}}]}}))
        time.sleep(x.ms / 1000.0)
        need(c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': pos + [
            {'type': 'btn', 'data': {'down': False, 'button': 'left'}}]}}))
        print('tap %d,%d (%d ms)' % (x.x, x.y, x.ms))
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
        need(c.call({'op': 'put', 'local': HOOK_EXE, 'remote': 'C:\\sys\\bin\\' + hook_name(), 'stamp': True}))
        need(agent('upgrade', ['C:\\data\\sydb\\devagent.new']))
        time.sleep(1.5)
        r = need(c.call({'op': 'wait', 'what': 'agent', 'timeout': 30}))
        if not r.get('agent'):
            die('agent did not come back')
        print('agent restarted (pid %s)' % r['hello'].get('pid'))
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
