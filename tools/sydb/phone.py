"""sydb phone: an emulator window for the virtual N95 (like the Android emulator's): the screen, the
phone's keys and machine controls, talking to a running sydb session (docs/phone-skin.md).

  tools/sydb/sydb.py phone [--scale 2]      (starts a session first if none is running)

- Screen: QEMU serves its display as VNC on <session>/vnc.sock; a minimal RFB 3.8 client (raw
  encoding, 32 bpp) reads it. Drawn pixel-exact (nearest) at --scale.
- Keys: mouse press/release on the buttons and the host keyboard send real key down/up events
  (daemon op keyev), so long presses work (hold Menu = task switcher, hold Power = switch off).
- Side toolbar: Power, Vol +/-, Landscape (media slide; Ctrl+Left/Right), Keypad slide, Shot,
  Snapshot, Pause, Reset. Landscape taps F13 (EKeyDeviceA: akncapserver switches the UI to landscape) and turns the view so the
  landscape UI is upright, as when the real phone is turned; closed/keypad tap F14 (EKeyDeviceB,
  portrait). The keypad slide itself sends no event yet (Nokia's keypad.dll is not in our ROM,
  see docs/phone-skin.md).
"""
import os, socket, struct, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sydb as S

from PyQt6 import QtCore, QtGui, QtWidgets
Qt = QtCore.Qt

# host keyboard -> phone keys (QMP qcodes); PC function keys as in docs/n95-controls.md
HOST_KEYS = {
    Qt.Key.Key_Up: 'up', Qt.Key.Key_Down: 'down', Qt.Key.Key_Left: 'left', Qt.Key.Key_Right: 'right',
    Qt.Key.Key_Return: 'f6', Qt.Key.Key_Enter: 'f6', Qt.Key.Key_Backspace: 'backspace',
    Qt.Key.Key_F2: 'f2', Qt.Key.Key_F3: 'f3', Qt.Key.Key_F4: 'f4', Qt.Key.Key_F5: 'f5', Qt.Key.Key_F6: 'f6',
    Qt.Key.Key_F7: 'f7', Qt.Key.Key_F8: 'f8', Qt.Key.Key_F9: 'f9', Qt.Key.Key_F10: 'f10',
    Qt.Key.Key_F11: 'f11', Qt.Key.Key_F12: 'f12', Qt.Key.Key_Asterisk: 'kp_multiply', Qt.Key.Key_NumberSign: 'f10',
    Qt.Key.Key_Space: 'spc', Qt.Key.Key_Escape: 'f7',
}
for i in range(10):
    HOST_KEYS[getattr(Qt.Key, 'Key_%d' % i)] = str(i)
for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    HOST_KEYS[getattr(Qt.Key, 'Key_' + c)] = c.lower()


class Rfb(QtCore.QObject):
    """minimal RFB 3.8 client over the session's unix socket: raw encoding, 32 bpp"""
    frame = QtCore.pyqtSignal(QtGui.QImage)
    status = QtCore.pyqtSignal(str)

    def __init__(self, path):
        super().__init__()
        self.path, self.stop = path, False
        threading.Thread(target=self.run, daemon=True).start()

    def recv(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError('VNC closed')
            b += c
        return bytes(b)

    def run(self):
        while not self.stop:
            try:
                self.session()
            except Exception as e:
                self.status.emit('screen: %s (retrying)' % e)
                time.sleep(1)

    def session(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(self.path)
        self.recv(12)
        self.s.sendall(b'RFB 003.008\n')
        n = self.recv(1)[0]
        types = self.recv(n)
        if 1 not in types:
            raise ConnectionError('VNC security types %r' % list(types))
        self.s.sendall(b'\x01')
        if struct.unpack('>I', self.recv(4))[0] != 0:
            raise ConnectionError('VNC security failed')
        self.s.sendall(b'\x01')                                   # shared
        w, h = struct.unpack('>HH', self.recv(4))
        self.recv(16)
        self.recv(struct.unpack('>I', self.recv(4))[0])
        # 32 bpp true colour, little endian, 0x00RRGGBB = QImage RGB32 bytes
        self.s.sendall(struct.pack('>B3xBBBBHHHBBB3x', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.s.sendall(struct.pack('>BxHii', 2, 2, 0, -223))      # Raw, DesktopSize
        self.w, self.h = w, h
        self.fb = bytearray(w * h * 4)
        self.status.emit('screen: %dx%d' % (w, h))
        self.request(False)
        while not self.stop:
            t = self.recv(1)[0]
            if t == 0:
                n = struct.unpack('>xH', self.recv(3))[0]
                for _ in range(n):
                    x, y, rw, rh, enc = struct.unpack('>HHHHi', self.recv(12))
                    if enc == 0:
                        data = self.recv(rw * rh * 4)
                        for row in range(rh):
                            o = ((y + row) * self.w + x) * 4
                            self.fb[o:o + rw * 4] = data[row * rw * 4:(row + 1) * rw * 4]
                    elif enc == -223:
                        self.w, self.h = rw, rh
                        self.fb = bytearray(rw * rh * 4)
                img = QtGui.QImage(bytes(self.fb), self.w, self.h, self.w * 4, QtGui.QImage.Format.Format_RGB32).copy()
                self.frame.emit(img)
                self.request(True)
            elif t == 1:
                _, _, n = struct.unpack('>xHH', self.recv(5))
                self.recv(n * 6)
            elif t == 2:
                pass
            elif t == 3:
                self.recv(struct.unpack('>3xI', self.recv(7))[0])
            else:
                raise ConnectionError('VNC message %d' % t)

    def request(self, incremental):
        self.s.sendall(struct.pack('>BBHHHH', 3, 1 if incremental else 0, 0, 0, self.w, self.h))


class Screen(QtWidgets.QWidget):
    def __init__(self, scale):
        super().__init__()
        self.scale, self.img, self.landscape = scale, None, False
        self.resize_for()

    def resize_for(self):
        w, h = (320, 240) if self.landscape else (240, 320)
        self.setFixedSize(int(w * self.scale), int(h * self.scale))

    def set_frame(self, img):
        self.img = img
        self.update()

    def paintEvent(self, e):
        p = QtGui.QPainter(self)
        p.fillRect(self.rect(), QtGui.QColor(0, 0, 0))
        if self.img:
            img = self.img
            if self.landscape:          # the media slide: the phone is turned. In landscape the
                # picture's top is at the screen's left edge (d_display stand-in), so turn it clockwise
                img = img.transformed(QtGui.QTransform().rotate(90))
            p.drawImage(self.rect(), img)


class Phone(QtWidgets.QWidget):
    def __init__(self, sdir, scale):
        super().__init__()
        self.sdir, self.c = sdir, S.Client(sdir)
        self.setWindowTitle('N95 8GB - sydb phone (%s)' % os.path.basename(sdir))
        self.setStyleSheet("""
            QWidget#body { background: #2b2b2e; border-radius: 28px; }
            QPushButton { background: #4a4a50; color: #eee; border: 1px solid #666; border-radius: 8px;
                          padding: 4px; min-height: 26px; }
            QPushButton:pressed { background: #8a8a95; }
            QPushButton:checked { background: #1565c0; border-color: #64b5f6; }
            QPushButton:disabled { color: #777; background: #38383c; }
            QPushButton#call { background: #2e7d32; } QPushButton#end { background: #b71c1c; }
            QLabel#status { color: #ccc; }
        """)
        outer = QtWidgets.QHBoxLayout(self)
        body = QtWidgets.QWidget(objectName='body')
        outer.addWidget(body)
        side = QtWidgets.QVBoxLayout()
        outer.addLayout(side)
        v = QtWidgets.QVBoxLayout(body)
        v.setContentsMargins(24, 24, 24, 24)
        self.screen = Screen(scale)
        v.addWidget(self.screen, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.status = QtWidgets.QLabel('connecting...', objectName='status')
        v.addWidget(self.status)
        # front keys
        g = QtWidgets.QGridLayout()
        self.key(g, 'LSK', 'f5', 0, 0); self.key(g, 'Menu', 'f8', 0, 2); self.key(g, 'RSK', 'f7', 0, 4)
        self.key(g, 'Call', 'f2', 1, 0, name='call'); self.key(g, '^', 'up', 1, 2); self.key(g, 'End', 'f3', 1, 4, name='end')
        self.key(g, '<', 'left', 2, 1); self.key(g, 'OK', 'f6', 2, 2); self.key(g, '>', 'right', 2, 3)
        self.key(g, 'C', 'backspace', 3, 0); self.key(g, 'v', 'down', 3, 2)
        mm = self.key(g, 'Multimedia', None, 3, 4)
        mm.setToolTip('Multimedia key: N95 scan code not mapped yet')
        v.addLayout(g)
        # slide-out keypad
        self.keypad = QtWidgets.QWidget()
        kg = QtWidgets.QGridLayout(self.keypad)
        for i, k in enumerate('123456789*0#'):
            self.key(kg, k, {'*': 'kp_multiply', '#': 'f10'}.get(k, k), i // 3, i % 3)
        v.addWidget(self.keypad)
        # media keys (the N95 8GB's upward slide)
        self.media = QtWidgets.QWidget()
        mg = QtWidgets.QHBoxLayout(self.media)
        for k in ('|<<', '>||', '[]', '>>|'):
            b = QtWidgets.QPushButton(k)
            b.setEnabled(False)
            b.setToolTip('media keys: N95 scan codes not mapped yet')
            mg.addWidget(b)
        v.addWidget(self.media)
        # side toolbar (like the Android emulator's): hardware keys, slides, machine
        for label, qc in (('Power', 'f4'), ('Vol +', 'f11'), ('Vol -', 'f12'), ('Camera', None)):
            b = self.key(None, label, qc)
            if qc is None:
                b.setToolTip('camera key: N95 scan code not mapped yet')
            side.addWidget(b)
        side.addSpacing(10)
        self.slide_state = 0
        self.land_btn = QtWidgets.QPushButton('Landscape\n(media slide)')
        self.land_btn.setToolTip('Open the media-key slide: the phone switches to landscape and the view '
                                 'turns with it (Ctrl+Left / Ctrl+Right)')
        self.pad_btn = QtWidgets.QPushButton('Keypad\nslide')
        self.pad_btn.setToolTip('Open the keypad slide (shows the number keys; the phone gets no slide event yet)')
        for b, i in ((self.land_btn, 2), (self.pad_btn, 1)):
            b.setCheckable(True)
            b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            b.clicked.connect(lambda on, i=i: self.slide(i if on else 0))
            side.addWidget(b)
        side.addSpacing(10)
        for label, fn in (('Shot', self.shot), ('Snapshot', self.snap), ('Pause', self.pause), ('Reset', self.reset)):
            b = QtWidgets.QPushButton(label)
            b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            b.clicked.connect(fn)
            side.addWidget(b)
            if label == 'Pause':
                self.pause_btn = b
        side.addStretch(1)
        self.slide(0)
        # screen + status
        self.rfb = Rfb(os.path.join(sdir, 'vnc.sock'))
        self.rfb.frame.connect(self.screen.set_frame)
        self.rfb.status.connect(lambda m: self.set_status(screen=m))
        self.st = {}
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(1500)
        self.held = set()
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def key(self, grid, label, qcode, row=0, col=0, name=None):
        b = QtWidgets.QPushButton(label, objectName=name or '')
        b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        if qcode is None:
            b.setEnabled(False)
        else:
            b.pressed.connect(lambda q=qcode: self.send(q, True))
            b.released.connect(lambda q=qcode: self.send(q, False))
        if grid is not None:
            grid.addWidget(b, row, col)
        return b

    def call(self, req, timeout=10):
        def go():
            try:
                self.c.call(req, timeout)
            except Exception as e:
                QtCore.QMetaObject.invokeMethod(self.status, 'setText', Qt.ConnectionType.QueuedConnection,
                                                QtCore.Q_ARG(str, 'error: %s' % e))
        threading.Thread(target=go, daemon=True).start()

    def send(self, qcode, down):
        self.call({'op': 'keyev', 'keys': [qcode], 'down': down})

    def tap(self, qcode, hold=0.15):
        def go():                          # down and up in order, from one thread
            try:
                self.c.call({'op': 'keyev', 'keys': [qcode], 'down': True})
                time.sleep(hold)
                self.c.call({'op': 'keyev', 'keys': [qcode], 'down': False})
            except Exception as e:
                QtCore.QMetaObject.invokeMethod(self.status, 'setText', Qt.ConnectionType.QueuedConnection,
                                                QtCore.Q_ARG(str, 'error: %s' % e))
        threading.Thread(target=go, daemon=True).start()

    def keyPressEvent(self, e):
        if e.modifiers() & Qt.KeyboardModifier.ControlModifier and e.key() in (Qt.Key.Key_Left, Qt.Key.Key_Right):
            if not e.isAutoRepeat():       # rotate, as in the Android emulator
                self.slide(0 if self.slide_state == 2 else 2)
            return
        q = HOST_KEYS.get(e.key())
        if q and not e.isAutoRepeat() and q not in self.held:
            self.held.add(q)
            self.send(q, True)

    def keyReleaseEvent(self, e):
        q = HOST_KEYS.get(e.key())
        if q and not e.isAutoRepeat() and q in self.held:
            self.held.discard(q)
            self.send(q, False)

    def focusOutEvent(self, e):
        for q in list(self.held):          # never leave a key held down
            self.send(q, False)
        self.held.clear()

    def slide(self, i, tell=True):
        """0 closed, 1 keypad slide open, 2 media slide open (landscape)"""
        if tell:                           # the layout switch the N95 makes for the media slide
            self.tap('f13' if i == 2 else 'f14')
        self.slide_state = i
        self.land_btn.setChecked(i == 2)
        self.pad_btn.setChecked(i == 1)
        self.keypad.setVisible(i == 1)
        self.media.setVisible(i == 2)
        self.screen.landscape = i == 2
        self.screen.resize_for()
        QtCore.QTimer.singleShot(0, self.fit)

    def fit(self):                         # follow the screen's size, shrinking back too
        self.layout().invalidate()
        self.layout().activate()
        self.resize(self.layout().totalMinimumSize())

    def reset(self):
        self.call({'op': 'qmp', 'cmd': 'system_reset'})
        self.slide(0, tell=False)          # the phone boots portrait, slides closed

    def pause(self):
        paused = self.pause_btn.text() == 'Resume'
        self.call({'op': 'vm', 'sub': 'resume' if paused else 'pause'})
        self.pause_btn.setText('Pause' if paused else 'Resume')

    def shot(self):
        path = os.path.join(self.sdir, 'shots', time.strftime('phone-%H%M%S.png'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if self.screen.img:
            self.screen.img.save(path)
            self.set_status(note='saved ' + path)

    def snap(self):
        self.call({'op': 'snap', 'sub': 'save', 'name': time.strftime('phone-%H%M%S')}, timeout=300)
        self.set_status(note='snapshot saved (sydb snap list)')

    def set_status(self, **kw):
        self.st.update(kw)
        self.status.setText('  '.join(str(v) for v in self.st.values() if v))

    def poll(self):
        def go():
            try:
                st = self.c.call({'op': 'status'}, 3)
                ag = st.get('agent') or {}
                txt = '%s  %.0fs  agent %s' % (st.get('state'), st.get('uptime', 0), 'up' if ag else 'down')
            except Exception as e:
                txt = 'session: %s' % e
            try:
                QtCore.QMetaObject.invokeMethod(self, '_status', Qt.ConnectionType.QueuedConnection, QtCore.Q_ARG(str, txt))
            except RuntimeError:           # the window closed meanwhile
                pass
        threading.Thread(target=go, daemon=True).start()

    @QtCore.pyqtSlot(str)
    def _status(self, txt):
        self.set_status(session=txt)


def main(sdir, scale=2.0):
    app = QtWidgets.QApplication(sys.argv[:1])
    w = Phone(sdir, scale)
    w.show()
    return app.exec()
