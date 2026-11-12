#!/usr/bin/env python3
"""The hosted EKA2 phone in a window: boots the device's user side on the hosted kernel (the test VM,
tools/hosted/htest.py --phone) and draws its screen from the shared memory the VM writes it into
(src/hosted/variant/hdisplay.cpp: header, key ring, XRGB8888 pixels). Closing the window ends the VM.

    python3 tools/hosted/phone.py [--rom DEVICE] [--scale 2]

Keys: the host keyboard as in the sydb phone window (tools/sydb/phone.py HOST_KEYS: arrows, Enter = OK,
F5/F7 = soft keys, 0-9, * and #, Backspace = C, ...), mapped to the device's scan codes by its keymap
(models/<model>/keymap.toml through tools/device.py qemu_keymap).

Ctrl+T opens the Linux console beside the phone: a `top` of the VM (src/hosted/init lxtop, every 2 s on the
serial log) with the one Linux process "symbian" and the Symbian threads as its Linux threads.
"""
import argparse, importlib.util, mmap, os, re, signal, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHM = os.path.join(ROOT, 'build/hosted/htest/phone.shm')
MAGIC = 0x42465048
PIXELS = 0x1000
KEY_HEAD, KEY_TAIL, KEY_RING, KEY_SLOTS = 0x100, 0x104, 0x108, 64
SERIAL = os.path.join(ROOT, 'build/hosted/htest/phone.log')
CONSOLE_W = 600


class TopReader:
    """the latest complete LXTOP block from the VM's serial log (lines between other output are skipped)"""
    def __init__(self, path):
        self.path, self.pos, self.partial, self.block, self.latest = path, 0, b'', [], []

    def poll(self):
        try:
            with open(self.path, 'rb') as f:
                if os.fstat(f.fileno()).st_size < self.pos:
                    self.pos = 0            # a new VM run
                f.seek(self.pos)
                data = f.read()
                self.pos += len(data)
        except OSError:
            return False
        lines = (self.partial + data).split(b'\n')
        self.partial = lines.pop()
        changed = False
        for l in lines:
            if l.startswith(b'LXTOP-END'):
                self.latest, self.block, changed = self.block, [], True
            elif l.startswith(b'LXTOP'):
                self.block.append(l[6:].decode('latin-1').rstrip())
        return changed


def key_codes(device):
    """Qt key -> Symbian scan code: sydb's Qt key -> QEMU qcode table, then the device keymap's qcode -> scan code"""
    spec = importlib.util.spec_from_file_location('sydb_phone', os.path.join(ROOT, 'tools/sydb/phone.py'))
    sp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sp)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import device as devmodel
    qcode = {}
    for line in devmodel.qemu_keymap(devmodel.load(device)):
        m = re.match(r'(\S+) (0x[0-9a-f]+)', line)
        if m:
            qcode.setdefault(m.group(1), int(m.group(2), 16))
    return {k: qcode[q] for k, q in sp.HOST_KEYS.items() if q in qcode}


def push_key(mm, code, pressed):
    """one entry into the key ring (the VM's display thread feeds it to the keyboard model); False when full"""
    head, tail = struct.unpack_from('<2I', mm, KEY_HEAD)
    if (head - tail) & 0xffffffff >= KEY_SLOTS:
        return False
    struct.pack_into('<I', mm, KEY_RING + 4 * (head % KEY_SLOTS), (code & 0xffff) | (0x80000000 if pressed else 0))
    struct.pack_into('<I', mm, KEY_HEAD, (head + 1) & 0xffffffff)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default=os.environ.get('SYBORG_DEVICE', 'nokia-n95-8gb-rm320'))
    ap.add_argument('--scale', type=int, default=2)
    a = ap.parse_args()

    from PyQt6 import QtCore, QtGui, QtWidgets

    # the VM (it creates the shared file before QEMU starts)
    if os.path.exists(SHM):
        os.unlink(SHM)
    vm = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools/hosted/htest.py'), '--rom', a.rom, '--phone',
                           '--shm', SHM], start_new_session=True)

    app = QtWidgets.QApplication(sys.argv)
    w, h = 240, 320
    keys = key_codes(a.rom)

    class Screen(QtWidgets.QWidget):
        def __init__(self):
            super().__init__()
            self.setWindowTitle('hosted %s' % a.rom)
            self.setFixedSize(w * a.scale, h * a.scale)
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)
            self.top = TopReader(SERIAL)
            self.console = False
            self.font = QtGui.QFont('monospace', 9)
            self.font.setStyleHint(QtGui.QFont.StyleHint.Monospace)
            self.mm = None
            self.frame = -1
            self.image = None
            t = QtCore.QTimer(self)
            t.timeout.connect(self.poll)
            t.start(20)

        def poll(self):
            if vm.poll() is not None:
                self.close()
                return
            if self.top.poll() and self.console:
                self.update()
            if self.mm is None:
                if not os.path.exists(SHM) or os.path.getsize(SHM) < (1 << 20):
                    return
                f = open(SHM, 'r+b')
                self.mm = mmap.mmap(f.fileno(), 1 << 20)
            magic, _ver, fw, fh, pitch, frame = struct.unpack_from('<6I', self.mm, 0)
            if magic != MAGIC or not fw or frame == self.frame:
                return
            self.frame = frame
            data = bytes(self.mm[PIXELS:PIXELS + pitch * fh])
            self.image = QtGui.QImage(data, fw, fh, pitch, QtGui.QImage.Format.Format_RGB32).copy()
            self.update()

        def key(self, e, pressed):
            code = keys.get(e.key())
            if code is None or e.isAutoRepeat() or self.mm is None:
                return False
            push_key(self.mm, code, pressed)
            return True

        def keyPressEvent(self, e):
            if e.key() == QtCore.Qt.Key.Key_T and e.modifiers() & QtCore.Qt.KeyboardModifier.ControlModifier:
                self.console = not self.console
                self.setFixedSize(w * a.scale + (CONSOLE_W if self.console else 0), h * a.scale)
                self.update()
                return
            if not self.key(e, True):
                super().keyPressEvent(e)

        def keyReleaseEvent(self, e):
            if not self.key(e, False):
                super().keyReleaseEvent(e)

        def paintEvent(self, _e):
            p = QtGui.QPainter(self)
            phone = QtCore.QRect(0, 0, w * a.scale, h * a.scale)
            if self.image is None:
                p.fillRect(phone, QtGui.QColor(0, 0, 0))
                p.setPen(QtGui.QColor(200, 200, 200))
                p.drawText(phone, QtCore.Qt.AlignmentFlag.AlignCenter, 'booting...  (Ctrl+T: Linux console)')
            else:
                p.drawImage(phone, self.image)
            if self.console:
                r = QtCore.QRect(phone.right() + 1, 0, CONSOLE_W, phone.height())
                p.fillRect(r, QtGui.QColor(0, 0, 0))
                p.setFont(self.font)
                p.setPen(QtGui.QColor(80, 255, 120))
                lh = QtGui.QFontMetrics(self.font).lineSpacing()
                lines = ['root@hosted-n95:~# top        (Ctrl+T: back)', ''] + (self.top.latest or ['waiting for the VM...'])
                for i, l in enumerate(lines):
                    y = 8 + (i + 1) * lh
                    if y > r.bottom():
                        break
                    p.drawText(r.left() + 8, y, l)

        def closeEvent(self, e):
            stop_vm()
            e.accept()

    def stop_vm():
        if vm.poll() is None:
            os.killpg(vm.pid, signal.SIGTERM)
            try:
                vm.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(vm.pid, signal.SIGKILL)

    s = Screen()
    s.show()
    # Ctrl-C / kill: close the window (and the VM) instead of leaving QEMU running
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: app.quit())
    tick = QtCore.QTimer()
    tick.timeout.connect(lambda: None)		# lets Python run its signal handlers inside Qt's loop
    tick.start(200)
    try:
        return app.exec()
    finally:
        stop_vm()


if __name__ == '__main__':
    sys.exit(main())
