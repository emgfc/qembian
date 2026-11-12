#!/usr/bin/env python3
"""ngage_phone: an emulator window for the virtual N-Gage QD (like `sydb phone` for the EKA2 phones).

  python3 eka1/tools/ngage_phone.py [--phone ngage] [--fresh] [--scale 2] [--keep]

One command: it starts the phone (an ngage session, eka1/tools/ngage_sess.py, named phone-NAME)
when it isn't running, shows the screen and the phone's keys, and ends the session when the window
closes (--keep leaves it running; a session it joined is never stopped).
The phone keeps its flash (C:, settings) in build/ngage/phones/NAME.flash between boots (Q55);
--fresh starts factory-fresh (the first boot formats C: and restores factory settings, ~75 s more).

- Screen: QMP screendump of the board's console, i.e. what the board shows (the window server's
  screen chunk, H-ngage-fb, or the panel), refreshed 10 times a second.
- Keys: the buttons and the host keyboard send real key down/up events (QMP input-send-event,
  QEMU key codes); the board maps them to the N-Gage keypad (ngage.c NGAGE_KEYS), so a long
  press is a long press.
  Host keyboard: arrows, Enter = select, F1/F2 = left/right softkey, F3 = call (green),
  F4 = end (red), F5 = menu, Backspace = clear, Shift = edit, 0-9, * and #, F12 = power.
"""
import argparse, os, pathlib, subprocess, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ngage_sess as ns  # noqa: E402

from PyQt6 import QtCore, QtGui, QtWidgets
Qt = QtCore.Qt

# (label, QEMU qcode) rows: the N-Gage QD's keys; the board maps each qcode (ngage.c NGAGE_KEYS)
KEY_ROWS = [
    [('Left soft', 'f1'), ('Menu', 'f5'), ('Right soft', 'f2')],
    [('Call', 'f3'), ('▲', 'up'), ('End', 'f4')],
    [('◀', 'left'), ('Select', 'ret'), ('▶', 'right')],
    [('C', 'backspace'), ('▼', 'down'), ('Power', 'f12')],
    [('1', '1'), ('2', '2'), ('3', '3')],
    [('4', '4'), ('5', '5'), ('6', '6')],
    [('7', '7'), ('8', '8'), ('9', '9')],
    [('*', 'kp_multiply'), ('0', '0'), ('#', 'f10')],
]

HOST_KEYS = {
    Qt.Key.Key_Up: 'up', Qt.Key.Key_Down: 'down', Qt.Key.Key_Left: 'left', Qt.Key.Key_Right: 'right',
    Qt.Key.Key_Return: 'ret', Qt.Key.Key_Enter: 'ret', Qt.Key.Key_Backspace: 'backspace',
    Qt.Key.Key_F1: 'f1', Qt.Key.Key_F2: 'f2', Qt.Key.Key_F3: 'f3', Qt.Key.Key_F4: 'f4',
    Qt.Key.Key_F5: 'f5', Qt.Key.Key_F12: 'f12', Qt.Key.Key_Asterisk: 'kp_multiply',
    Qt.Key.Key_NumberSign: 'f10', Qt.Key.Key_Shift: 'shift',
}
for i in range(10):
    HOST_KEYS[getattr(Qt.Key, 'Key_%d' % i)] = str(i)


class Phone(QtWidgets.QWidget):
    def __init__(self, session, scale):
        super().__init__()
        self.session, self.scale = session, scale
        self.qmp = ns.Qmp(session)
        self.shot = str(ns.sdir(session) / 'screen.ppm')
        self.setWindowTitle('N-Gage QD (ngage, session %s)' % session)
        lay = QtWidgets.QVBoxLayout(self)
        self.screen = QtWidgets.QLabel()
        self.screen.setFixedSize(176 * scale, 208 * scale)
        self.screen.setStyleSheet('background: black')
        lay.addWidget(self.screen, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.status = QtWidgets.QLabel('booting...')
        lay.addWidget(self.status)
        grid = QtWidgets.QGridLayout()
        for r, row in enumerate(KEY_ROWS):
            for c, (label, code) in enumerate(row):
                b = QtWidgets.QPushButton(label)
                b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
                b.pressed.connect(lambda code=code: self.key(code, True))
                b.released.connect(lambda code=code: self.key(code, False))
                grid.addWidget(b, r, c)
        lay.addLayout(grid)
        self.t0 = time.time()
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(100)

    def key(self, code, down):
        try:
            self.qmp.cmd('input-send-event', events=[
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}])
        except OSError as e:
            self.status.setText('key: %s' % e)

    def keyPressEvent(self, e):
        code = HOST_KEYS.get(e.key())
        if code and not e.isAutoRepeat():
            self.key(code, True)

    def keyReleaseEvent(self, e):
        code = HOST_KEYS.get(e.key())
        if code and not e.isAutoRepeat():
            self.key(code, False)

    def tick(self):
        try:
            r = self.qmp.cmd('screendump', filename=self.shot)
        except OSError as e:
            self.status.setText('QEMU gone: %s' % e)
            self.timer.stop()
            return
        if 'error' in r:
            return
        img = QtGui.QImage(self.shot)
        if not img.isNull():
            self.screen.setPixmap(QtGui.QPixmap.fromImage(img).scaled(
                176 * self.scale, 208 * self.scale, Qt.AspectRatioMode.IgnoreAspectRatio,
                Qt.TransformationMode.FastTransformation))
        st = ns.call(self.session, {'op': 'status'}, 2)
        hit = st.get('hit') or {}
        self.status.setText('%s, %.0f s%s' % (st['state'], time.time() - self.t0,
                            ('  (%s: see %s)' % (hit.get('label'), hit.get('dump', ''))) if hit else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scale', type=int, default=2)
    ap.add_argument('--phone', default='ngage', help='which phone (its flash file)')
    ap.add_argument('--mmc', default='', help='memory card image (E:), made with eka1/tools/ngage_card.py')
    ap.add_argument('--fresh', action='store_true', help='factory-fresh: forget the saved flash')
    ap.add_argument('--keep', action='store_true', help='leave the session running when the window closes')
    a = ap.parse_args()
    a.session = 'phone-' + a.phone
    flash = ns.ROOT / 'build/ngage/phones' / (a.phone + '.flash')
    flash.parent.mkdir(parents=True, exist_ok=True)
    started = False
    try:
        running = ns.call(a.session, {'op': 'status'}, 2)['state'] in ('running', 'frozen', 'paused')
    except OSError:
        running = False
    if running and a.fresh:
        subprocess.run([sys.executable, ns.__file__, '-s', a.session, 'stop'], stdout=subprocess.DEVNULL)
        running = False
    if a.fresh and flash.exists():
        flash.unlink()
    if not running:
        # crash catches only: a fault freezes the machine and the status line names the dump
        subprocess.run([sys.executable, ns.__file__, '-s', a.session, 'start', '--flash', str(flash)]
                       + (['--mmc', os.path.abspath(a.mmc)] if a.mmc else []),
                       check=True, stdout=subprocess.DEVNULL)
        started = True
    app = QtWidgets.QApplication(sys.argv)
    import signal
    for sig in (signal.SIGINT, signal.SIGTERM):        # Ctrl+C closes the window like the X button
        signal.signal(sig, lambda *_: app.quit())
    w = Phone(a.session, a.scale)
    w.show()
    rc = app.exec()
    if started and not a.keep:
        subprocess.run([sys.executable, ns.__file__, '-s', a.session, 'stop'], stdout=subprocess.DEVNULL)
    return rc


if __name__ == '__main__':
    sys.exit(main())
