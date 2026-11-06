"""sydb tui: a small curses view over a running session (tools/sydb/sydb.py tui).

  top     status: phone state, uptime, agent, focused window group
  left    window groups (named ones, front first)       right  session events
  middle  serial log tail (filter with ':log REGEX', ':log' to clear)
  bottom  command line: any sydb command ("ps Calc", "launch Clock", "key menu down ok",
          "screenshot", "dbg attach" ...); output replaces the log pane until the next refresh.
          Phone keys without typing: Tab enters key mode (arrows, Enter = ok, F5/F7/F8 pass
          through, Esc leaves). q or :q quits.
"""
import curses, json, os, shlex, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sydb as S

KEYMODE = {curses.KEY_UP: 'up', curses.KEY_DOWN: 'down', curses.KEY_LEFT: 'left', curses.KEY_RIGHT: 'right',
           10: 'ok', curses.KEY_ENTER: 'ok', curses.KEY_F5: 'lsk', curses.KEY_F7: 'rsk', curses.KEY_F8: 'menu',
           curses.KEY_BACKSPACE: 'clear', 127: 'clear', ord('*'): 'star', ord('#'): 'hash'}


def run(stdscr, sdir):
    c = S.Client(sdir)
    curses.curs_set(1)
    stdscr.nodelay(True)
    curses.use_default_colors()
    for i, col in enumerate((curses.COLOR_GREEN, curses.COLOR_YELLOW, curses.COLOR_CYAN, curses.COLOR_RED), 1):
        curses.init_pair(i, col, -1)
    cmd, out, out_t, log_filter, keymode = '', [], 0, None, False
    last, data = 0, {}
    while True:
        now = time.time()
        if now - last > 1.5:
            last = now
            try:
                data['st'] = c.call({'op': 'status'}, timeout=3)
                data['ev'] = c.call({'op': 'events', 'since': max(0, data['st'].get('events', 0) - 40)}, timeout=3)['events']
                data['log'] = c.call({'op': 'log', 'grep': log_filter, 'max': 200}, timeout=3)['lines']
                w = c.call({'op': 'agent', 'cmd': 'wg', 'args': [], 'timeout': 3}, timeout=5)
                data['wg'] = [i for i in w.get('items', []) if i.get('name')] if w.get('ok') else data.get('wg', [])
                data['focus'] = w.get('focus_name', '?')
            except (OSError, RuntimeError, ValueError) as e:
                data['err'] = str(e)
        H, W = stdscr.getmaxyx()
        stdscr.erase()
        st = data.get('st', {})
        ag = st.get('agent') or {}
        line = ' sydb  %s  %.0fs  agent %s  focus %s  %s' % (
            st.get('state', '?'), st.get('uptime', 0), ('v%s pid %s' % (ag.get('version'), ag.get('pid'))) if ag else 'down',
            data.get('focus', '?'), '[KEYS: arrows Enter F5 F7 F8, Esc]' if keymode else '')
        stdscr.addnstr(0, 0, line.ljust(W), W, curses.A_REVERSE)
        top_h = max(6, (H - 4) // 3)
        half = W // 2
        stdscr.addnstr(1, 0, ' window groups', half - 1, curses.color_pair(3) | curses.A_BOLD)
        for i, g in enumerate(data.get('wg', [])[:top_h - 1]):
            name = str(g.get('name', '')).split('|')
            label = name[2] if len(name) > 2 and name[2].strip() else (name[1] if len(name) > 1 else name[0])
            stdscr.addnstr(2 + i, 0, '%s%-5s %5s %s' % ('*' if g.get('focus') else ' ', g['wgid'], g.get('prio', ''), label),
                           half - 1, curses.color_pair(1) if g.get('focus') else 0)
        stdscr.addnstr(1, half, ' events', W - half - 1, curses.color_pair(3) | curses.A_BOLD)
        evs = data.get('ev', [])[-(top_h - 1):]
        for i, e in enumerate(evs):
            rest = {k: v for k, v in e.items() if k not in ('seq', 't', 'kind')}
            col = curses.color_pair(4) if 'panic' in e['kind'] else curses.color_pair(2)
            stdscr.addnstr(2 + i, half, '%7.1f %s %s' % (e['t'], e['kind'], json.dumps(rest)[:200]), W - half - 1, col)
        y0 = top_h + 1
        body = out if out and now - out_t < 30 else data.get('log', [])
        title = ' output' if out and now - out_t < 30 else ' serial log' + (' /%s/' % log_filter if log_filter else '')
        stdscr.addnstr(y0, 0, title, W - 1, curses.color_pair(3) | curses.A_BOLD)
        avail = H - y0 - 3
        for i, l in enumerate(body[-avail:] if avail > 0 else []):
            stdscr.addnstr(y0 + 1 + i, 0, l, W - 1)
        if data.get('err'):
            stdscr.addnstr(H - 2, 0, 'error: ' + data['err'], W - 1, curses.color_pair(4))
        stdscr.addnstr(H - 1, 0, ('keys> ' if keymode else 'sydb> ') + cmd, W - 1)
        stdscr.refresh()
        try:
            ch = stdscr.get_wch()
        except curses.error:
            time.sleep(0.05)
            continue
        if keymode:
            code = ch if isinstance(ch, int) else (10 if ch == '\n' else ord(ch))
            if code == 27:
                keymode = False
            elif code == 9:
                keymode = False
            elif code in KEYMODE:
                c.call({'op': 'key', 'keys': [KEYMODE[code]], 'delay': 0.05})
            elif isinstance(ch, str) and ch.isalnum():
                c.call({'op': 'key', 'keys': [ch.lower()], 'delay': 0.05})
            last = 0
            continue
        if ch in ('\n', curses.KEY_ENTER):
            text, cmd = cmd.strip(), ''
            if text in ('q', ':q', 'quit', 'exit'):
                return
            if text.startswith(':log'):
                log_filter = text[4:].strip() or None
                out, last = [], 0
                continue
            if text:
                p = subprocess.run([sys.executable, os.path.join(HERE, 'sydb.py'), '--session', sdir] + shlex.split(text),
                                   capture_output=True, text=True, timeout=600)
                out, out_t, last = ['$ ' + text] + (p.stdout + p.stderr).splitlines(), time.time(), 0
        elif ch == '\t':
            keymode = True
        elif ch in (curses.KEY_BACKSPACE, '\b', '\x7f'):
            cmd = cmd[:-1]
        elif ch == 'q' and not cmd:
            return
        elif isinstance(ch, str) and ch.isprintable():
            cmd += ch


def main(sdir):
    if not S.Client(sdir).alive():
        sys.exit('sydb: no session running (sydb start)')
    curses.wrapper(run, sdir)
