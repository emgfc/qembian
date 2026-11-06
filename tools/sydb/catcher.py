"""sydb catch: freeze, dump and snapshot the phone the moment a thread dies the way a rule says.

Two cold kernel/user paths are breakpointed through the persistent gdb (debugger.py), so normal
running is not slowed down:
  User::Panic(const TDesC16&, TInt)   user mode, before the panic exec call: the panicking thread
                                       is frozen alive with all its registers (User::RaiseException,
                                       CBase/USER panics and leaves that end in a panic all get here)
  DThread::Die(TExitType, TInt, const TDesC8&)   kernel: every thread exit, also those the kernel
                                       decides (KERN-EXEC 3 for an exception, kills by other threads);
                                       user registers come from the kernel's user-context tables
A stop is matched against the rules (kind, thread/process regex, exact category, reason); no
match = continue at once. A match writes a dump (registers, symbolised call-stack scan, user stack
bytes, recent log) to <session>/catches/, raises a catch.hit event and then, per rule:
  action dump      continue
  action freeze    stay stopped (inspect with dbg_*; catch_resume or dbg continue goes on)
  snapshot=true    also save a machine snapshot (session.snapshot), e.g. to fork the moment
"""
import json, os, re, struct, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel  # tools/device.py: build paths of the device ($SYBORG_DEVICE / devices/default)
DEVPATHS = devmodel.paths(devmodel.load())
USER_PANIC = 0x803a9f8a          # EUser.dll _ZN4User5PanicERK7TDesC16i (Thumb 0x803a9f8b), N95 ROM
DIE = 'DThread::Die'
EXIT_TYPES = {0: 'kill', 1: 'terminate', 2: 'panic', 3: 'pending'}
CONTEXT_TABLES = 'ThreadUserContextTables'
REGNAMES = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12', 'sp', 'lr', 'pc', 'cpsr']
N95_USER_START, ROM_END = 0x8038f9a0, 0x84000000
CORE_FLAT = DEVPATHS['core_flat']


class Catcher:
    def __init__(self, sess, dbg):
        self.sess, self.dbg = sess, dbg
        self.rules, self.hits = [], []
        self.next_rule = 1
        self.bps = {}                # gdb breakpoint number -> 'panic' | 'die'
        self.frozen = None           # hit dict while the phone is held
        self.lock = threading.Lock()
        self.recent = {}             # thread name -> time of a User::Panic hit (skip its Die)
        self.dir = os.path.join(sess.sdir, 'catches')
        self.core = None
        self.stats = {'stops': 0, 'matched': 0}
        self.busy = False

    # ---- rules ----
    def add(self, rule):
        at = rule.get('at')
        if at is not None:          # breakpoint rule: dump (or freeze) every time this address runs
            at = int(str(at), 0) & ~1
        r = {'id': self.next_rule, 'kind': 'break' if at is not None else rule.get('kind', 'panic'),
             'at': '0x%08x' % at if at is not None else None, 'max': int(rule.get('max') or 20), 'thread': rule.get('thread'),
             'category': rule.get('category'), 'reason': rule.get('reason'), 'action': rule.get('action', 'dump'),
             'snapshot': bool(rule.get('snapshot')), 'once': bool(rule.get('once')), 'hits': 0}
        if r['kind'] not in ('panic', 'kill', 'terminate', 'exit', 'any', 'break'):
            raise ValueError('kind must be panic, kill, terminate, exit (abnormal exits) or any')
        if r['action'] not in ('dump', 'freeze'):
            raise ValueError('action must be dump or freeze')
        if r['thread']:
            re.compile(r['thread'])
        self.next_rule += 1
        self.rules.append(r)
        self.arm()
        if at is not None:
            self.insert_bp('*0x%x' % at, 'at:%d' % r['id'])
        return r

    def insert_bp(self, loc, what):
        d = self.dbg
        was = d.running
        if was:
            d.mi('-exec-interrupt', timeout=5)
            d.wait(5)
        r = d.mi('-break-insert -h %s' % loc)
        self.bps[int(r['bkpt']['number'])] = what
        if was and not self.frozen:
            d.mi('-exec-continue')

    def remove(self, rid=None):
        gone = [r for r in self.rules if rid is None or r['id'] == rid]
        self.rules = [r for r in self.rules if rid is not None and r['id'] != rid]
        for r in gone:                 # drop a breakpoint rule's own breakpoint
            for n, what in list(self.bps.items()):
                if what == 'at:%d' % r['id'] and self.rules:
                    self.delete_bp(n)
        if not self.rules:
            self.disarm()
        return {'rules': self.rules}

    def arm(self):
        """attach gdb if needed, insert the two breakpoints, let the phone run"""
        d = self.dbg
        if not d.p or d.p.poll() is not None:
            d.start()
        if not self.bps:
            if d.running:
                d.mi('-exec-interrupt', timeout=5)
                d.wait(5)
            for loc, what in (('*0x%x' % USER_PANIC, 'panic'), (DIE, 'die')):
                r = d.mi('-break-insert -h %s' % loc)
                self.bps[int(r['bkpt']['number'])] = what
        d.catcher = self
        if not d.running and not self.frozen:
            d.mi('-exec-continue')

    def disarm(self):
        d = self.dbg
        if not self.bps or not d.p:
            return
        was = d.running
        if d.running:
            d.mi('-exec-interrupt', timeout=5)
            d.wait(5)
        for n in list(self.bps):
            try:
                d.mi('-break-delete %d' % n)
            except Exception:
                pass
        self.bps = {}
        if was or self.frozen:
            self.frozen = None
            d.mi('-exec-continue')

    def delete_bp(self, n):
        d = self.dbg
        was = d.running
        if was:
            d.mi('-exec-interrupt', timeout=5)
            d.wait(5)
        try:
            d.mi('-break-delete %d' % n)
        except Exception:
            pass
        self.bps.pop(n, None)
        if was and not self.frozen:
            d.mi('-exec-continue')

    def owns(self, st):
        return st.get('reason') == 'breakpoint-hit' and st.get('bkptno') and int(st['bkptno']) in self.bps

    # ---- a stop at one of our breakpoints (debugger worker thread) ----
    def handle(self, st):
        d = self.dbg
        if self.frozen or self.busy:
            # a repeated stop report while a hit is held (e.g. the VM stop around a snapshot):
            # not a new event, and never a reason to resume
            return
        self.busy = True
        try:
            self._handle(st)
        finally:
            self.busy = False

    def _handle(self, st):
        d = self.dbg
        what = self.bps.get(int(st.get('bkptno', 0)))
        self.stats['stops'] += 1
        ev = self.read_event(what)
        rule = self.match(ev)
        if not rule:
            self._go()
            return
        self.stats['matched'] += 1
        rule['hits'] += 1
        if rule['once']:
            self.rules.remove(rule)
        if what == 'panic':
            self.recent[ev['thread']] = time.time()
        hit = self.dump(ev, rule)
        if rule['snapshot']:
            try:
                hit['snapshot'] = self.sess.snapshot('catch-%d' % hit['id'], keep_stopped=True)['name']
            except Exception as e:
                hit['snapshot_error'] = str(e)
        self.sess.event('catch.hit', id=hit['id'], rule=rule['id'], thread=ev['thread'], exit=ev['kind'],
                        category=ev['category'], reason=ev['reason'], at=hit.get('at'), caller=hit.get('caller'),
                        frozen=rule['action'] == 'freeze', snapshot=hit.get('snapshot'))
        try:
            self.sess.gc(forks=False)        # retention: newest 5 automatic snapshots, 200 dumps
        except Exception as e:
            self.sess.event('catch.error', error='gc: %s' % e)
        if rule['action'] == 'freeze':
            self.frozen = hit
            d.stops.put({'reason': 'catch', 'bkptno': st.get('bkptno')})   # dbg wait returns it
        else:
            self._go()
        if not self.rules:
            self.disarm()

    def _go(self):
        """continue, dropping stop reports queued while we held the machine (nothing real can
        have happened while it was stopped)"""
        q = self.dbg.catch_q
        while not q.empty():
            q.get_nowait()
        if not self.dbg.running:
            self.dbg.mi('-exec-continue')

    def resume(self):
        if not self.frozen:
            return {'running': True}
        self.frozen = None
        self._go()
        return {'running': True}

    # ---- what happened ----
    def read_event(self, what):
        d = self.dbg
        if what.startswith('at:'):
            return {'source': 'break', 'kind': 'break', 'rule_id': int(what[3:]), 'category': None, 'reason': None,
                    'thread': d.thread_name(), 'dthread': None}
        r0, r1, r2, r3 = (d.reg(x) for x in ('r0', 'r1', 'r2', 'r3'))
        if what == 'panic':      # User::Panic(const TDesC16& aCategory, TInt aReason), user mode
            return {'source': 'User::Panic', 'kind': 'panic', 'category': d.read_des(r0, True), 'reason': _s32(r1),
                    'thread': d.thread_name(), 'dthread': None}
        # DThread::Die(this, TExitType, TInt, const TDesC8&)
        kind = EXIT_TYPES.get(r1, str(r1))
        cat = d.read_des(r3, False)
        name = d.thread_name(r0)
        return {'source': 'DThread::Die', 'kind': kind, 'category': cat, 'reason': _s32(r2), 'thread': name, 'dthread': r0}

    def match(self, ev):
        if ev['source'] == 'DThread::Die' and time.time() - self.recent.get(ev['thread'], 0) < 5:
            return None          # already handled at its User::Panic
        for r in self.rules:
            k = r['kind']
            if ev['kind'] == 'break' or k == 'break':
                if ev['kind'] == 'break' and k == 'break' and r['id'] == ev['rule_id'] and r['hits'] < r['max'] \
                        and (not r['thread'] or re.search(r['thread'], ev['thread'], re.I)):
                    return r
                continue
            if k == 'panic' and ev['kind'] != 'panic':
                continue
            if k in ('kill', 'terminate') and ev['kind'] != k:
                continue
            if k == 'exit' and ev['kind'] == 'kill' and ev['reason'] == 0:
                continue         # 'exit' = any abnormal exit
            if r['thread'] and not re.search(r['thread'], ev['thread'], re.I):
                continue
            if r['category'] and r['category'] != ev['category']:
                continue
            if r['reason'] is not None and int(r['reason']) != ev['reason']:
                continue
            return r
        return None

    # ---- the dump ----
    def user_context(self, ev):
        """user registers: from gdb at User::Panic (user mode), else from the kernel's tables"""
        d = self.dbg
        if ev['source'] in ('User::Panic', 'break'):
            regs = {n: d.reg(n) for n in REGNAMES}
            return regs, 'exact (at the breakpoint)'
        t = ev['dthread']
        nt = d.evalx('&((DThread*)0x%x)->iNThread' % t)
        ctype = d.evalx('((DThread*)0x%x)->iNThread.iSpare3' % t) & 0xff
        base = d.evalx('((DThread*)0x%x)->iNThread.iStackBase' % t)
        size = d.evalx('((DThread*)0x%x)->iNThread.iStackSize' % t)
        saved = d.evalx('((DThread*)0x%x)->iNThread.iSavedSP' % t)
        current = d.evalx('(unsigned)TheScheduler.iCurrentThread') == nt
        if current and ctype not in (1,):        # in an exec call: the kernel reports EContextExec (11)
            ctype = 11
        tables = d.evalx('&%s' % CONTEXT_TABLES)
        table = d.u32(tables + 4 * ctype)
        if not table:
            return {}, 'no user context (type %d)' % ctype
        raw = d.read_mem(table, 2 * len(REGNAMES))
        top = base + size
        regs = {}
        for i, n in enumerate(REGNAMES):
            typ, val = raw[2 * i], raw[2 * i + 1]
            if typ == 1 and not current:          # EOffsetFromSp (valid when switched out)
                regs[n] = d.u32(saved + 4 * val)
            elif typ == 2:                        # EOffsetFromStackTop
                regs[n] = d.u32(top - 4 * val)
            elif typ == 3 and not current:        # ESpPlusOffset
                regs[n] = saved + 4 * val
        note = {1: 'exception frame', 11: 'exec call (sp/lr not saved while running)'}.get(ctype, 'context type %d' % ctype)
        return regs, note

    def code_map(self):
        """RAM-loaded code segments: N95CODE lines of this boot"""
        segs = []
        for l in self.sess.log_query(r'N95CODE: ', 0, 2000)['lines']:
            m = re.search(r'N95CODE: (\S+) code=([0-9a-f]+)\+([0-9a-f]+)', l)
            if m:
                segs.append((int(m.group(2), 16), int(m.group(3), 16), os.path.basename(m.group(1).replace('\\', '/'))))
        return segs

    def sym(self, a, segs):
        for base, size, name in segs:
            if base <= a < base + size:
                return '%s+%x' % (name, a - base)
        if 0x80000000 <= a < ROM_END:
            return self.dbg.romsym(a) or '0x%08x' % a
        return None

    def code_bytes(self, a, n):
        """n bytes of code at a: the N95 core image for the N95 user side, else target memory"""
        if N95_USER_START <= a < ROM_END and os.path.exists(CORE_FLAT):
            if self.core is None:
                self.core = open(CORE_FLAT, 'rb').read()
            o = a - 0x80000000
            return self.core[o:o + n]
        return self.dbg.read_mem(a, n)

    def is_return_address(self, a, segs):
        """does the instruction before a look like a call (BL/BLX/mov lr,pc)?"""
        if not (0x80000000 <= a < ROM_END or any(b <= a < b + s for b, s, _ in segs)):
            return False
        if a & 1:                                   # Thumb
            b = self.code_bytes((a & ~1) - 4, 4)
            if len(b) < 4:
                return False
            h1, h2 = struct.unpack('<HH', b)
            if (h1 & 0xf800) == 0xf000 and (h2 & 0xe800) in (0xe800, 0xf800):
                return True                         # BL / BLX pair
            return (h2 & 0xff87) == 0x4780          # BLX reg
        if a & 3:
            return False
        b = self.code_bytes(a - 8, 8)
        if len(b) < 8:
            return False
        w0, w1 = struct.unpack('<II', b)
        return ((w1 & 0x0f000000) == 0x0b000000 or (w1 & 0xfe000000) == 0xfa000000 or
                (w1 & 0x0ffffff0) == 0x012fff30 or w0 == 0xe1a0e00f)

    def dump(self, ev, rule):
        d = self.dbg
        os.makedirs(self.dir, exist_ok=True)
        hid = len(self.hits) + 1
        segs = self.code_map()
        try:
            regs, note = self.user_context(ev)
        except Exception as e:          # e.g. the thread's memory is not mapped right now
            regs, note = {}, 'user context not readable: %s' % e
        hit = {'id': hid, 't': self.sess.now(), 'rule': rule['id'], **{k: v for k, v in ev.items() if k != 'dthread'},
               'regs_note': note, 'regs': {k: '0x%08x' % v for k, v in regs.items() if v is not None}}
        pc, lr, sp = regs.get('pc'), regs.get('lr'), regs.get('sp')
        if pc:
            hit['at'] = self.sym(pc, segs)
        if lr:
            hit['caller'] = self.sym(lr, segs)
        # the user stack: from sp (or the whole stack) to its top
        stack_lo = stack_hi = None
        try:
            t = ev['dthread'] or d.evalx(debugger_thread_expr())
            stack_lo = d.evalx('((DThread*)0x%x)->iUserStackRunAddress' % t)
            stack_hi = stack_lo + d.evalx('((DThread*)0x%x)->iUserStackSize' % t)
        except Exception:
            stack_hi = None
        frames, raw = [], b''
        if stack_hi:
            lo = sp if sp and stack_lo <= sp < stack_hi else max(stack_lo, stack_hi - 0x4000)
            lo = max(lo, stack_hi - 0x8000)
            try:
                raw = d.read_mem(lo, stack_hi - lo)
            except Exception as e:
                # a thread of another process (killed from outside): its user memory is not in the
                # current address space
                hit['stack_note'] = 'user stack not readable (%s); thread not current?' % e
                raw = b''
            for i in range(0, len(raw) - 3, 4):
                w = struct.unpack_from('<I', raw, i)[0]
                try:
                    ok = self.is_return_address(w, segs)
                except Exception:
                    ok = False
                if ok:
                    frames.append({'sp': '0x%08x' % (lo + i), 'ret': '0x%08x' % w, 'sym': self.sym(w, segs)})
                    if len(frames) >= 48:
                        break
            hit['stack'] = {'lo': '0x%08x' % lo, 'hi': '0x%08x' % stack_hi, 'from_sp': bool(sp and lo == sp),
                            'bytes': len(raw)}
            if not raw and 'stack_note' not in hit:
                hit['stack_note'] = 'user stack read returned no data (memory not mapped in the current address space?)'
        hit['frames'] = frames
        hit['log_tail'] = self.sess.log_query(None, 0, 60)['lines']
        base = os.path.join(self.dir, '%03d-%s' % (hid, re.sub(r'[^\w.-]+', '_', ev['thread'])[:40]))
        hit['files'] = {'json': base + '.json', 'txt': base + '.txt', 'stack': base + '.stack.bin'}
        if raw:
            open(base + '.stack.bin', 'wb').write(raw)
        json.dump(hit, open(base + '.json', 'w'), indent=1)
        open(base + '.txt', 'w').write(render(hit))
        self.hits.append({k: hit[k] for k in ('id', 't', 'rule', 'thread', 'kind', 'category', 'reason', 'source')
                          if k in hit} | {'at': hit.get('at'), 'caller': hit.get('caller'), 'files': hit['files']})
        return hit

    def status(self):
        return {'rules': self.rules, 'hits': len(self.hits), 'frozen': self.frozen and self.frozen['id'],
                'armed': bool(self.bps), **self.stats}


def debugger_thread_expr():
    import debugger
    return debugger.THREAD_EXPR


def _s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def render(h):
    out = ['catch hit %d at %.3f s (rule %d, %s)' % (h['id'], h['t'], h['rule'], h['source']),
           '  thread   %s' % h['thread'],
           '  exit     %s %s %s' % (h['kind'], h['category'], h['reason']),
           '  at       %s' % h.get('at'), '  caller   %s' % h.get('caller'),
           '  regs     (%s)' % h['regs_note']]
    regs = h['regs']
    for i in range(0, len(REGNAMES), 6):
        out.append('    ' + '  '.join('%-4s %s' % (n, regs.get(n, '-')) for n in REGNAMES[i:i + 6]))
    if h.get('stack_note'):
        out.append('  stack    ' + h['stack_note'])
    out.append('  call-stack scan (words on the stack whose preceding instruction is a call):')
    for f in h['frames']:
        out.append('    %s  %s  %s' % (f['sp'], f['ret'], f['sym']))
    out.append('  log tail:')
    out += ['    ' + l for l in h['log_tail'][-25:]]
    return '\n'.join(out) + '\n'
