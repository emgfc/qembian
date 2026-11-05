#!/usr/bin/env python3
"""Minimal QMP client for the modern-QEMU Syborg runs (stdlib only).

  qmp.py SOCK screendump OUT.png
  qmp.py SOCK type "text\\n"            (US keyboard layout; '\\n' = Enter)
  qmp.py SOCK key ctrl-alt-delete      (one chord, QKeyCode names joined by '-')
  qmp.py SOCK savevm TAG | loadvm TAG  (HMP via human-monitor-command)
  qmp.py SOCK hmp "info jit"
"""
import json, socket, sys, time

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
           '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
           '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave_accent'}
PLAIN = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '[': 'bracket_left',
         ']': 'bracket_right', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe',
         ',': 'comma', '.': 'dot', '/': 'slash', '`': 'grave_accent', '\t': 'tab'}


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rw')
        json.loads(self.f.readline())           # greeting
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'event' in r:
                continue
            if 'error' in r:
                raise SystemExit('%s: %s' % (name, r['error']))
            return r.get('return')


def keys_for(ch):
    if ch.isalpha():
        return (['shift'] if ch.isupper() else []) + [ch.lower()]
    if ch.isdigit():
        return [ch]
    if ch in SHIFTED:
        return ['shift', SHIFTED[ch]]
    if ch in PLAIN:
        return [PLAIN[ch]]
    raise SystemExit('cannot type %r' % ch)


def send(q, names):
    q.cmd('send-key', keys=[{'type': 'qcode', 'data': n} for n in names], **{'hold-time': 80})
    time.sleep(0.15)


def main():
    sock, op, *rest = sys.argv[1:]
    q = Qmp(sock)
    if op == 'screendump':
        q.cmd('screendump', filename=rest[0], format='png')
    elif op == 'type':
        for ch in rest[0].encode().decode('unicode_escape'):
            send(q, keys_for(ch))
    elif op == 'key':
        send(q, rest[0].split('-'))
    elif op in ('savevm', 'loadvm'):
        print(q.cmd('human-monitor-command', **{'command-line': '%s %s' % (op, rest[0])}))
    elif op == 'hmp':
        print(q.cmd('human-monitor-command', **{'command-line': rest[0]}))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
