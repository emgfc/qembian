"""sydb phone: an emulator window for the virtual N95 (like the Android emulator's): the screen, the
phone's keys and machine controls, talking to a running sydb session (docs/phone-skin.md).

  tools/sydb/sydb.py phone [--scale S]      (starts a session first if none is running; default
                                             scale 1)

- Screen: read straight from guest RAM (the session backs it with a shared file) where the Syborg
  fb registers say the picture is. Drawn pixel-exact at whole scales, filtered at fractional ones.
- Keys: mouse press/release on the buttons and the host keyboard send real key down/up events
  (daemon op keyev), so long presses work (hold Menu = task switcher, hold Power = switch off).
- Side toolbar: Power, Vol +/-, Landscape (media slide; Ctrl+Left/Right), Keypad slide, Shot,
  Snapshot, Pause, Reset. Landscape taps F13 (EKeyDeviceA: akncapserver switches the UI to landscape) and turns the view so the
  landscape UI is upright, as when the real phone is turned; closed/keypad tap F14 (EKeyDeviceB,
  portrait). Turn view (Ctrl+T) turns the picture 90 degrees anticlockwise (touch follows); on phones with
  the accelerometer stand-in (N8, 701, 808) it also sets the host sensor (syborg-sensor) so the phone's
  automatic rotation sees the turn; elsewhere no event: for games that keep the portrait screen and draw
  landscape themselves (FixNativeOrientation, e.g. Angry Birds). The keypad slide itself sends no event yet (Nokia's keypad.dll is not in our ROM,
  see docs/phone-skin.md).
"""
import mmap, os, queue, re, socket, struct, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sydb as S

from PyQt6 import QtCore, QtGui, QtWidgets
Qt = QtCore.Qt

# host keyboard -> phone keys (QMP qcodes); PC function keys as in docs/devices/nokia-n95-8gb-rm320/n95-controls.md
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


class ShmFb(QtCore.QObject):
    """the screen straight from guest RAM: the session backs RAM with a shared file (daemon
    ram_path), the Syborg fb registers (polled through the daemon) say where the picture is and
    how the controller shows it. Does what syborg_fb.c does: rotation, partial band, dimming."""
    frame = QtCore.pyqtSignal(QtGui.QImage)
    status = QtCore.pyqtSignal(str)
    DIM = 25                                   # syborg-fb blank-dim default (percent)

    def __init__(self, client, path, parent=None):
        super().__init__(parent)
        self.c, self.regs, self.path, self.mm = client, None, path, None
        # host-composed frames (syborg-gl frame-file): a sequence, odd while a frame is written
        self.seq_path = os.path.join(client.sdir, 'frame.seq')
        self.seq_mm, self.seq_last, self.seq_t = None, None, 0.0
        threading.Thread(target=self.poll_regs, daemon=True).start()
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(16)                   # ~60 Hz, the panel's refresh

    def poll_regs(self):
        while True:
            try:
                out = self.c.call({'op': 'hmp', 'cmd': 'xp /17wx 0xc0005000'}, 3)['out']
                # "c0005000: 0x.. 0x.. 0x.. 0x.." lines: word 0 = ID, 1 BASE, 2 HEIGHT, 3 WIDTH, ...
                self.regs = [int(v, 16) for line in out.splitlines()
                             for v in re.findall(r'0x([0-9a-f]{8})', line.split(':', 1)[-1])]
                self.status.emit('screen: shared memory')
            except Exception as e:
                self.regs = None
                self.status.emit('screen: %s' % e)
            time.sleep(0.25)

    def tick(self):
        r = self.regs
        if not r or len(r) < 17:
            return
        if self.mm is None:                    # QEMU creates the file as it starts
            try:
                with open(self.path, 'rb') as f:
                    self.mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            except (OSError, ValueError):
                return
        if self.seq_mm is None and os.path.exists(self.seq_path):
            try:
                with open(self.seq_path, 'rb') as f:   # header, then the host-composed screen at 4096
                    self.seq_mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            except (OSError, ValueError):
                self.seq_mm = None
        seq = None
        if self.seq_mm is not None and self.seq_mm[:4] == b'FSEQ':
            seq = struct.unpack_from('<I', self.seq_mm, 4)[0]
            if seq:                            # the device composes on the host
                now = time.monotonic()
                if seq & 1:
                    return                     # a frame is being written: next tick
                if seq == self.seq_last and now - self.seq_t < 0.25:
                    return                     # nothing new (still repaint now and then: a guest
                self.seq_t = now               # may also draw into the frame buffer itself)
            else:
                seq = None
        base, height, width, rot, blank, bpp, pitch, enabled = r[1], r[2], r[3], r[4] & 3, r[5], r[8], r[12], r[13]
        pstart, pend, pmode = r[14], r[15], r[16]
        if not base or bpp != 32 or not width or not height:
            return
        pw, ph = (height, width) if rot & 1 else (width, height)
        pitch = pitch or pw * 4
        if base + pitch * ph > len(self.mm):
            return
        # the host-composed screen (syborg-gl keeps it on the host: the guest's display memory is then the
        # surface DSA apps draw into), else guest memory
        hw, hh, hon = struct.unpack_from('<III', self.seq_mm, 8) if seq is not None else (0, 0, 0)
        if hon == 1 and hw == pw and hh == ph and 4096 + pw * ph * 4 <= len(self.seq_mm):
            img = QtGui.QImage(self.seq_mm[4096:4096 + pw * ph * 4], pw, ph, pw * 4,
                               QtGui.QImage.Format.Format_RGB32).copy()
        else:
            img = QtGui.QImage(self.mm[base:base + pitch * ph], pw, ph, pitch, QtGui.QImage.Format.Format_RGB32).copy()
        if seq is not None:                    # torn copy (a frame was written meanwhile): drop it
            if struct.unpack_from('<I', self.seq_mm, 4)[0] != seq:
                return
            self.seq_last = seq
        if pmode & 1 or not enabled or blank:
            p = QtGui.QPainter(img)
            if pmode & 1:                      # partial display: only the band is lit
                black = QtGui.QColor(0, 0, 0)
                if pmode & 4:                  # band = picture rows
                    p.fillRect(0, 0, pw, pstart, black); p.fillRect(0, pend + 1, pw, ph - pend - 1, black)
                else:                          # band = picture columns
                    p.fillRect(0, 0, pstart, ph, black); p.fillRect(pend + 1, 0, pw - pend - 1, ph, black)
            if not enabled or blank:           # no backlight: dimmed
                p.fillRect(0, 0, pw, ph, QtGui.QColor(0, 0, 0, 255 * (100 - self.DIM) // 100))
            p.end()
        if rot:                                # quarter turns clockwise onto the screen
            img = img.transformed(QtGui.QTransform().rotate(90 * rot))
        self.frame.emit(img)


class Screen(QtWidgets.QWidget):
    """the phone's screen; on touch phones ([board.display] touch) the mouse is the finger: press,
    drag and release go to the guest as absolute pointer events (syborg-pointer -> exyin stand-in)"""
    def __init__(self, scale, size=(240, 320), touch=None):
        super().__init__()
        self.scale, self.img, self.landscape, self.turned = scale, None, False, False
        self.size_px, self.touch = size, touch          # touch: callable(x 0..1, y 0..1, down) or None
        if touch:
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            self.setMouseTracking(False)                 # moves only while the button is held (a finger)
        self.resize_for()

    def resize_for(self):
        w, h = self.size_px
        w, h = (h, w) if self.landscape or self.turned else (w, h)
        self.setFixedSize(int(w * self.scale), int(h * self.scale))

    def finger(self, e, down):
        if not self.touch or e.button() not in (Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton):
            return
        x, y = e.position().x() / max(1, self.width()), e.position().y() / max(1, self.height())
        if self.landscape:          # the picture is turned clockwise on screen: undo it for the guest
            x, y = y, 1.0 - x
        elif self.turned:           # turned anticlockwise (Turn view)
            x, y = 1.0 - y, x
        self.touch(min(max(x, 0.0), 1.0), min(max(y, 0.0), 1.0), down)

    def mousePressEvent(self, e):
        self.finger(e, True)

    def mouseMoveEvent(self, e):
        if e.buttons() & Qt.MouseButton.LeftButton:
            self.finger(e, True)

    def mouseReleaseEvent(self, e):
        self.finger(e, False)

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
            elif self.turned:           # Turn view: anticlockwise, for games drawn landscape on the portrait screen
                img = img.transformed(QtGui.QTransform().rotate(-90))
            # whole device pixels per guest pixel: exact copies; otherwise filtered (nearest at a
            # fractional scale drops and doubles rows and columns unevenly: jagged text and lines)
            k = self.scale * self.devicePixelRatioF()
            if abs(k - round(k)) > 1e-3:
                p.setRenderHint(QtGui.QPainter.RenderHint.SmoothPixmapTransform)
            p.drawImage(self.rect(), img)


class Phone(QtWidgets.QWidget):
    def __init__(self, sdir, scale):
        super().__init__()
        self.sdir, self.c = sdir, S.Client(sdir)
        self.buttons = {}                  # qcode -> buttons, so host keys show on the phone
        prof = self.device_profile()
        disp = prof.get('board', {}).get('display', {})
        self.setWindowTitle('%s - sydb phone (%s)' % (prof.get('device', {}).get('model', 'phone'), os.path.basename(sdir)))
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
        self.ptr_q = queue.Queue()         # pointer events in order, one sender thread
        if disp.get('touch'):
            threading.Thread(target=self.pointer_sender, daemon=True).start()
        self.screen = Screen(scale, (disp.get('width', 240), disp.get('height', 320)),
                             self.pointer if disp.get('touch') else None)
        v.addWidget(self.screen, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.status = QtWidgets.QLabel('connecting...', objectName='status')
        v.addWidget(self.status)
        # the phone's keys, from the device keymap (models/<id>/keymap.toml via tools/device.py):
        # groups front / nav (grids by row, col), keypad (the keypad slide), media (the media slide),
        # side (the side column); keys without a host key or in "hidden" get no button
        km = self.device_keymap()
        groups = {}
        for k in km.get('keys', []):
            if k.get('host') and k.get('group') not in (None, 'hidden'):
                groups.setdefault(k['group'], []).append(k)
        def grid(keys, cols=3):
            g = QtWidgets.QGridLayout()
            for i, k in enumerate(keys):
                r, c = (k['row'], k['col']) if 'row' in k and 'col' in k else divmod(i, cols)
                b = self.key(g, k.get('label', k['key']), k['host'][0], r, c, name=k.get('style'))
                b.setToolTip('%s: %s, host key %s' % (k['key'], k['scancode'], ' / '.join(k['host'])))
            return g
        v.addLayout(grid(groups.get('front', [])))
        nav = QtWidgets.QHBoxLayout()
        nav.addStretch(1); nav.addLayout(grid(groups.get('nav', []))); nav.addStretch(1)
        v.addLayout(nav)
        self.keypad = QtWidgets.QWidget()
        self.keypad.setLayout(grid(groups.get('keypad', [])))
        v.addWidget(self.keypad)
        self.media = QtWidgets.QWidget()
        self.media.setLayout(grid(groups.get('media', []), cols=8))
        v.addWidget(self.media)
        # side toolbar (like the Android emulator's): hardware keys, slides, machine
        for k in groups.get('side', []):
            b = self.key(None, k.get('label', k['key']), k['host'][0])
            b.setToolTip('%s: %s, host key %s' % (k['key'], k['scancode'], ' / '.join(k['host'])))
            side.addWidget(b)
        side.addSpacing(10)
        self.slide_state = 0
        self.land_btn = QtWidgets.QPushButton('Landscape\n(media slide)')
        self.land_btn.setToolTip('Open the media-key slide: the phone switches to landscape and the view '
                                 'turns with it (Ctrl+Left / Ctrl+Right)')
        self.pad_btn = QtWidgets.QPushButton('Keypad\nslide')
        self.pad_btn.setToolTip('Open the keypad slide (shows the number keys; the phone gets no slide event yet)')
        # only the slides the phone has ([hardware] slides; the N95: keypad, media; the N8: none). Their
        # host keys (F13/F14) are other keys on phones without them (the N8's lock key is F13)
        try:
            slides = self.device_profile().get('hardware', {}).get('slides', [])
        except Exception:
            slides = []
        self.has_media, self.has_keypad = 'media' in slides, 'keypad' in slides
        for b, i, have in ((self.land_btn, 2, self.has_media), (self.pad_btn, 1, self.has_keypad)):
            b.setCheckable(True)
            b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            b.clicked.connect(lambda on, i=i: self.slide(i if on else 0))
            side.addWidget(b)
            b.setVisible(have)
        # phones with the accelerometer stand-in (N8, 701, 808) feel the turn: the sensor reports it
        try:
            self.has_accel = any(st.get('file', st.get('name')) == 'accelerometer'
                                 for st in self.device_profile().get('standins', []))
        except Exception:
            self.has_accel = False
        self.turn_btn = QtWidgets.QPushButton('Turn view\n(Ctrl+T)')
        self.turn_btn.setToolTip('Turn the picture 90 degrees anticlockwise (touch follows)' +
                                 ('; the accelerometer reports the turn, so the UI rotates where the phone '
                                  'rotates it' if self.has_accel else '; the phone gets no event. For games '
                                  'that draw landscape on the portrait screen.'))
        self.turn_btn.setCheckable(True)
        self.turn_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.turn_btn.clicked.connect(self.turn)
        side.addWidget(self.turn_btn)
        QtGui.QShortcut(QtGui.QKeySequence('Ctrl+T'), self, activated=lambda: self.turn(not self.screen.turned))
        side.addSpacing(10)
        for label, fn in (('GPU capture (30 frames)', self.gpu_capture), ('Shot', self.shot), ('Snapshot', self.snap), ('Pause', self.pause), ('Reset', self.reset),
                          ('Operator settings\n(SMS)', self.operator_settings)):
            b = QtWidgets.QPushButton(label)
            b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            b.clicked.connect(fn)
            side.addWidget(b)
            if label == 'Pause':
                self.pause_btn = b
        side.addStretch(1)
        self.slide(0)
        # screen + status
        ram = None
        try:
            ram = self.c.call({'op': 'status'}, 5).get('ram')
        except Exception:
            pass
        if not ram:
            raise SystemExit('sydb phone: the session has no shared guest RAM (restart it with this sydb)')
        self.fb = ShmFb(self.c, ram, self)        # the frame straight from guest RAM
        self.fb.frame.connect(self.screen.set_frame)
        self.fb.status.connect(lambda m: self.set_status(screen=m))
        self.st = {}
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(1500)
        self.held = set()
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def device_profile(self):
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), ''))
        import device as devmodel          # tools/device.py
        dev = None
        try:
            dev = self.c.call({'op': 'status'}, 5).get('device')
        except Exception:
            pass
        return devmodel.load(dev)

    def pointer(self, x, y, down):
        """finger at (x, y) in 0..1 of the screen, pressed or released: QMP absolute input"""
        self.ptr_q.put((int(x * 0x7fff), int(y * 0x7fff), down))

    def pointer_sender(self):
        """one QMP call per batch: moves queued while the previous call ran are merged into the latest
        position (a drag must not fall behind the mouse, nor its release arrive late and turn a flick
        into a long press); every press and release is sent, at its own position"""
        was = False
        while True:
            items = [self.ptr_q.get()]
            try:
                while True:
                    items.append(self.ptr_q.get_nowait())
            except queue.Empty:
                pass
            batch, state = [], was           # (x, y, down, is_move)
            for x, y, down in items:
                move = down == state
                if move and batch and batch[-1][3]:
                    batch[-1] = (x, y, down, True)
                else:
                    batch.append((x, y, down, move))
                state = down
            for x, y, down, _ in batch:
                ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': x}},
                      {'type': 'abs', 'data': {'axis': 'y', 'value': y}}]
                if down != was:
                    ev.append({'type': 'btn', 'data': {'down': down, 'button': 'left'}})
                    was = down
                try:
                    self.c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': ev}}, 5)
                except Exception as e:
                    QtCore.QMetaObject.invokeMethod(self.status, 'setText', Qt.ConnectionType.QueuedConnection,
                                                    QtCore.Q_ARG(str, 'touch: %s' % e))

    def device_keymap(self):
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), ''))
        import device as devmodel          # tools/device.py
        dev = None
        try:
            dev = self.c.call({'op': 'status'}, 5).get('device')
        except Exception:
            pass
        return devmodel.keymap(devmodel.load(dev))

    def key(self, grid, label, qcode, row=0, col=0, name=None):     # grid: grid or box layout
        b = QtWidgets.QPushButton(label, objectName=name or '')
        b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        if qcode is None:
            b.setEnabled(False)
        else:
            b.pressed.connect(lambda q=qcode: self.send(q, True))
            b.released.connect(lambda q=qcode: self.send(q, False))
            self.buttons.setdefault(qcode, []).append(b)
        if isinstance(grid, QtWidgets.QGridLayout):
            grid.addWidget(b, row, col)
        elif grid is not None:
            grid.addWidget(b)
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
            if not e.isAutoRepeat() and self.has_media:       # rotate, as in the Android emulator
                self.slide(0 if self.slide_state == 2 else 2)
            return
        q = HOST_KEYS.get(e.key())
        if q and not e.isAutoRepeat() and q not in self.held:
            self.held.add(q)
            self.send(q, True)
            self.show_key(q, True)

    def keyReleaseEvent(self, e):
        q = HOST_KEYS.get(e.key())
        if q and not e.isAutoRepeat() and q in self.held:
            self.held.discard(q)
            self.send(q, False)
            self.show_key(q, False)

    def show_key(self, qcode, down):
        # the on-screen key follows the host key: pressed while it is held (visual only: setDown
        # emits no pressed/released, so the key event is not sent twice)
        for b in self.buttons.get(qcode, []):
            b.setDown(down)

    def focusOutEvent(self, e):
        for q in list(self.held):          # never leave a key held down
            self.send(q, False)
            self.show_key(q, False)
        self.held.clear()

    def slide(self, i, tell=True):
        """0 closed, 1 keypad slide open, 2 media slide open (landscape)"""
        if tell and (self.has_media or self.has_keypad):    # the N95's layout switch for its slides
            self.tap('f13' if i == 2 else 'f14')
        self.slide_state = i
        self.land_btn.setChecked(i == 2)
        self.pad_btn.setChecked(i == 1)
        self.keypad.setVisible(i == 1)
        self.media.setVisible(i == 2)
        self.screen.landscape = i == 2
        self.screen.resize_for()
        QtCore.QTimer.singleShot(0, self.fit)

    def turn(self, on):
        """Turn view: the picture 90 degrees anticlockwise; with an accelerometer the phone is told it was
        turned (top to the left: gravity reaction +x), as when the real phone is turned"""
        if self.has_accel:
            v = '1000,0,0' if on else '0,1000,0'
            threading.Thread(target=lambda: self.c.call({'op': 'qmp', 'cmd': 'qom-set', 'args': {
                'path': '/machine/sensor', 'property': 'accel', 'value': v}}, 5), daemon=True).start()
        self.screen.turned = bool(on)
        self.turn_btn.setChecked(bool(on))
        self.screen.resize_for()
        QtCore.QTimer.singleShot(0, self.fit)

    def fit(self):                         # follow the screen's size, shrinking back too
        self.layout().invalidate()
        self.layout().activate()
        self.resize(self.layout().totalMinimumSize())

    @QtCore.pyqtSlot(str)
    def _note(self, txt):
        self.set_status(note=txt)

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

    def operator_settings(self):
        """the network operator's "settings over the air" message (OMA Client Provisioning by SMS) on the virtual
        SIM: the phone offers to save an Internet access point (tools/sms_provision.py, syborg-modem "sms")"""
        def go():
            try:
                sys.path.insert(0, os.path.dirname(HERE))
                import sms_provision
                for t in sms_provision.sms_deliver_tpdus(
                        sms_provision.wsp_push(sms_provision.provisioning_doc('internet', 'Internet')), '12345'):
                    r = self.c.call({'op': 'qmp', 'cmd': 'qom-set', 'args': {'path': '/machine/modem',
                                                                              'property': 'sms', 'value': t.hex()}}, 10)
                    if not r.get('ok'):
                        raise RuntimeError(r.get('error', 'failed'))
                message = 'operator settings: sent (save them on the phone)'
            except Exception as e:
                message = 'operator settings: %s' % e
            QtCore.QMetaObject.invokeMethod(self, '_capture_status', Qt.ConnectionType.QueuedConnection,
                                            QtCore.Q_ARG(str, message))
        threading.Thread(target=go, daemon=True).start()

    def gpu_capture(self):
        def go():
            try:
                result = self.c.call({'op': 'gpu_capture'}, 120)
                if 'path' not in result:
                    raise RuntimeError(str(result))
                self.gpu_capture_pending = True
                message = 'GPU capture: ' + result['path']
            except Exception as e:
                message = 'GPU capture: ' + str(e)
            QtCore.QMetaObject.invokeMethod(self, '_capture_status', Qt.ConnectionType.QueuedConnection,
                                            QtCore.Q_ARG(str, message))
        threading.Thread(target=go, daemon=True).start()

    @QtCore.pyqtSlot(str)
    def _capture_status(self, message):
        self.set_status(capture=message)

    def set_status(self, **kw):
        self.st.update(kw)
        self.status.setText('  '.join(str(v) for v in self.st.values() if v))

    def poll(self):
        def go():
            try:
                st = self.c.call({'op': 'status'}, 3)
                ag = st.get('agent') or {}
                if getattr(self, 'gpu_capture_pending', False):
                    capture = self.c.call({'op': 'qmp', 'cmd': 'qom-get',
                                           'args': {'path': '/machine/gl', 'property': 'capture'}}, 3)
                    value = capture.get('return', str(capture))
                    QtCore.QMetaObject.invokeMethod(self, '_capture_status', Qt.ConnectionType.QueuedConnection,
                                                    QtCore.Q_ARG(str, 'GPU capture: ' + value))
                    if not value.startswith('recording:'):
                        self.gpu_capture_pending = False
                        return
                txt = '%s  %.0fs  agent %s' % (st.get('state'), st.get('uptime', 0), 'up' if ag else 'down')
                self.gone = 0
                self.seen = True               # the session was up: from now on, gone = switched off
            except Exception as e:
                txt = 'session: %s' % e
                self.gone = getattr(self, 'gone', 0) + 1
            try:
                QtCore.QMetaObject.invokeMethod(self, '_status', Qt.ConnectionType.QueuedConnection, QtCore.Q_ARG(str, txt))
                if self.gone >= 2 and getattr(self, 'seen', False):   # switched off (or its session stopped)
                    QtCore.QMetaObject.invokeMethod(self, 'close', Qt.ConnectionType.QueuedConnection)
            except RuntimeError:           # the window closed meanwhile
                pass
        threading.Thread(target=go, daemon=True).start()

    @QtCore.pyqtSlot(str)
    def _status(self, txt):
        self.set_status(session=txt)


def main(sdir, scale=None):
    import signal
    app = QtWidgets.QApplication(sys.argv[:1])
    w = Phone(sdir, scale or 1.0)
    w.show()
    # Ctrl+C in the terminal closes the window like its close button (a KeyboardInterrupt raised
    # inside a Qt slot aborts the process); the timer lets Python see the signal while Qt runs
    signal.signal(signal.SIGINT, lambda *a: w.close())
    tick = QtCore.QTimer()
    tick.timeout.connect(lambda: None)
    tick.start(200)
    return app.exec()
