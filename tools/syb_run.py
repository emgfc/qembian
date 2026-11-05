#!/usr/bin/env python3
"""syb-run: boot the N95 hybrid on modern QEMU, observe, and summarise in one command.

  syb_run.py [--secs N] [--trace MASK] [--no-compose] [--shot T]... [--key T:KEYS]...
             [--type T:TEXT]... [--out DIR] [--label NAME] [--gui] [--report-args ...]

  --secs N        run time (default 30)
  --trace MASK    kernel trace mask for the composed ROM (default 0x80000C40 = panics + IPC trace;
                  0x80000000 = quiet/fast). Ignored with --no-compose.
  --shot T        screenshot at T seconds (repeatable; a final one is always taken)
  --key T:K       send a key chord at T seconds, QKeyCode names joined by '-' (e.g. 30:ret, 31:ctrl-c)
  --type T:TEXT   type TEXT at T seconds
  --out DIR       output directory (default build/runs/<label>)

Writes DIR/serial.log (each line prefixed with seconds since start), DIR/shot-<T>.png,
DIR/report.txt (tools/syb_report.py) and prints the report.
"""
import argparse, os, shutil, signal, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--secs', type=float, default=30)
    ap.add_argument('--trace', default='0x80000C40')
    ap.add_argument('--rom', default=os.path.join(ROOT, 'build', 'n95', 'hybrid.img'),
                    help='ROM image to compose into and boot (default: the working build/n95/hybrid.img)')
    ap.add_argument('--patches', help='byte-patch file for the compose (default rom/hybrid/code-patches.txt)')
    ap.add_argument('--no-compose', action='store_true')
    ap.add_argument('--shot', type=float, action='append', default=[])
    ap.add_argument('--key', action='append', default=[])
    ap.add_argument('--type', action='append', default=[])
    ap.add_argument('--out')
    ap.add_argument('--label', default=time.strftime('run-%H%M%S'))
    ap.add_argument('--gui', action='store_true')
    ap.add_argument('--report-args', nargs='*', default=[])
    a = ap.parse_args()
    global ROM_PATH
    ROM_PATH = a.rom
    out = os.path.abspath(a.out or os.path.join(ROOT, 'build', 'runs', a.label))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    if not a.no_compose:
        env = dict(os.environ, HYBRID_IMG=os.path.abspath(a.rom))
        if a.patches:
            env['HYBRID_PATCHES'] = os.path.abspath(a.patches)
        r = subprocess.run([os.path.join(ROOT, 'scripts', 'hybrid-boot.sh'), '--compose-only',
                            '--trace', a.trace], cwd=ROOT, capture_output=True, text=True, env=env)
        if r.returncode:
            sys.exit('compose failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    subprocess.run(['pkill', '-f', 'qemu-system-arm.*-name sybtool'])	# only our own QEMUs
    time.sleep(0.5)
    qmp = os.path.join(out, 'qmp.sock')
    cmd = [os.path.join(ROOT, 'scripts', 'run-modern.sh'), '--qmp', qmp, '--name', 'sybtool',
           '--nvmem', os.path.join(out, 'nvmemory.img'),
           '--dtb', os.path.join(ROOT, 'build', 'n95', 'syborg-n95.dtb')]
    if a.gui:
        cmd.append('--gui')
    cmd.append(os.path.abspath(ROM_PATH))
    # sound goes to a file in the run folder (the N95 fake DSP plays through syborg-audio)
    cmd += ['-audiodev', 'wav,id=snd0,path=' + os.path.join(out, 'audio.wav'), '-machine', 'audiodev=snd0']
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    t0 = time.time()
    log = open(os.path.join(out, 'serial.log'), 'w')

    def pump():
        for raw in p.stdout:
            log.write('%.3f %s' % (time.time() - t0, raw.decode('latin-1')))
        log.flush()
    th = threading.Thread(target=pump, daemon=True)
    th.start()

    events = [(t, 'shot', None) for t in a.shot]
    for spec in a.key:
        t, k = spec.split(':', 1); events.append((float(t), 'key', k))
    for spec in a.type:
        t, k = spec.split(':', 1); events.append((float(t), 'type', k))
    events.sort()
    qmp_tool = os.path.join(ROOT, 'tools', 'qmp.py')
    for t, kind, arg in events:
        if t > a.secs:
            break
        time.sleep(max(0, t - (time.time() - t0)))
        if kind == 'shot':
            subprocess.run([sys.executable, qmp_tool, qmp, 'screendump', os.path.join(out, 'shot-%g.png' % t)])
        elif kind == 'key':
            subprocess.run([sys.executable, qmp_tool, qmp, 'key', arg])
        elif kind == 'type':
            subprocess.run([sys.executable, qmp_tool, qmp, 'type', arg])
    time.sleep(max(0, a.secs - (time.time() - t0)))
    subprocess.run([sys.executable, qmp_tool, qmp, 'screendump', os.path.join(out, 'shot-final.png')])
    os.killpg(p.pid, signal.SIGKILL)
    try:
        os.remove(os.path.join(out, 'nvmemory.img'))	# the run's C: drive
    except OSError:
        pass
    p.wait()
    th.join(2)
    log.close()
    rep = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'syb_report.py'),
                          os.path.join(out, 'serial.log')] + a.report_args,
                         capture_output=True, text=True).stdout
    open(os.path.join(out, 'report.txt'), 'w').write(rep)
    print(rep)
    print('# outputs in ' + out)


if __name__ == '__main__':
    main()
