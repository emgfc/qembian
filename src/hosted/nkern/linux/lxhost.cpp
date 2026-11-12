// Hosted EKA2 nanokernel: host (Linux) primitives the platform layer is built on.
// - TLxEvent: auto-reset event (futex), the Win32 CreateEvent(FALSE, FALSE) used as schedule lock
// - TLxSemaphore: counting semaphore (futex)
// - SLxThread: one per host thread we run; TPIDRURO points at it (LxSelf); raw host threads start with 0
// - LxSuspend/LxResume: Win32 SuspendThread/ResumeThread with a count. Suspending signals the thread
//   (KLxSuspendSignal); its handler parks on the count and keeps the saved context so that the
//   scheduler can divert the thread (ApplyDiversion edits the context's pc).
// - LxCreateThread: a raw clone thread on an mmap'd stack, optionally created suspended.
// Original work for qembian; no Symbian code.

#define __INCLUDE_NTHREADBASE_DEFINES__
#include <nk_priv.h>

static const TInt KLxSuspendSignal = SIGUSR2;
enum { ELxFree = 0, ELxLive = 1, ELxDead = 2 };
static const TUint KLxStackSize = 0x20000;
static const TUint KLxAltSize = 0x4000;		// per-thread signal stack for fault dumps (stack overflows)	// 128 KB per host thread (kernel stacks are 4-8 KB on the phone)

static inline TInt AtomicLoad(volatile TInt* a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }

void TLxEvent::Set()
	{
	if (__atomic_exchange_n(&iState, 1, __ATOMIC_SEQ_CST) == 0)
		hfutex_wake(&iState, 1);
	}

void TLxEvent::Wait()
	{
	for (;;)
		{
		TInt one = 1;
		if (__atomic_compare_exchange_n(&iState, &one, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return;
		hfutex_wait(&iState, 0);
		}
	}

void TLxSemaphore::Signal()
	{
	__atomic_add_fetch(&iCount, 1, __ATOMIC_SEQ_CST);
	hfutex_wake(&iCount, 1);
	}

void TLxSemaphore::Wait()
	{
	for (;;)
		{
		TInt v = AtomicLoad(&iCount);
		if (v > 0)
			{
			if (__atomic_compare_exchange_n(&iCount, &v, v - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
				return;
			continue;
			}
		hfutex_wait(&iCount, v);
		}
	}

// ---- per host thread record ----
struct SLxThreadFull : public SLxThread
	{
	volatile TInt iGen;		// incremented by each 0 -> 1 suspend
	volatile TInt iAckGen;	// the generation the parked handler has acknowledged
	volatile TInt iSeq;		// bumped after every change of iSuspend/iGen: the parked thread sleeps on it
	volatile TInt iCtid;	// cleared by Linux when the thread has fully exited (CLONE_CHILD_CLEARTID)
	volatile TInt iState;	// ELxFree, ELxLive, ELxDead (exiting: stack reclaimed once iCtid == 0)
	SLxThreadFull* iNextAll;	// registry of every host thread (for the watchdog dump)
	SLxThreadFull* volatile iSuspending;	// debugging: the thread this one is suspending right now
	TLxThreadFunction iFunc;
	TAny* iArg;
	TUint8* iStack;
	TUint8* iAltStack;
	};

static void SetAltStack(TUint8* aBase, TUint aSize);

// The host thread's record is found through TPIDRURO (c13,c0,3: user read-only, set with the ARM
// set_tls call). TPIDRURW (c13,c0,2) belongs to the Symbian user side: EKA2 points it at the thread's
// TLocalThreadData (ARMv6K+ EUSER, e.g. Belle). Until 2026-11-12 the record was in TPIDRURW.
static TBool LxInitDone;

// NULL until LxInit: TPIDRURO holds a stale value at process start (see LxInit). Bug 2026-11-12: board
// code (the modem's register access) asked LxSelf() whether it runs on a nanothread before LxInit and
// dereferenced the stale value (kn_modem, one CPU). Regression: nk_lxinit.
SLxThread* LxSelf()
	{
	if (!LxInitDone)
		return NULL;
	TUint32 v;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
	return (SLxThread*)v;
	}

static void LxSetSelf(SLxThread* aT)
	{
	hsys(SYS_ARM_set_tls, aT);
	}

// Park while suspended. Sleep on iSeq, which every LxSuspend/LxResume bumps after changing the count
// and the generation, so no change can be missed. Bug 2026-11-12 (one CPU): the handler slept on
// iSuspend itself; a resume + re-suspend before it ran left the count at 1, the new generation was
// published after it had re-read it, and both sides waited forever. Regression: nk_lxsuspend.
static void Park(SLxThreadFull* me)
	{
	for (;;)
		{
		TInt q = AtomicLoad(&me->iSeq);
		TInt s = AtomicLoad(&me->iSuspend);
		__atomic_store_n(&me->iAckGen, AtomicLoad(&me->iGen), __ATOMIC_SEQ_CST);
		hfutex_wake(&me->iAckGen, 8);
		if (s <= 0)
			break;
		hfutex_wait(&me->iSeq, q);
		}
	}

static void SuspendHandler(int, hsiginfo*, hucontext* aUc)
	{
	SLxThreadFull* me = static_cast<SLxThreadFull*>(LxSelf());
	if (!me)
		return;
	me->iCtx = aUc;
	Park(me);
	me->iCtx = 0;
	}

static void Poke(SLxThreadFull* t)
	{
	__atomic_add_fetch(&t->iSeq, 1, __ATOMIC_SEQ_CST);
	hfutex_wake(&t->iSeq, 8);
	}

// Returns the previous suspend count, or -1 if the thread is gone (exited or exiting): like Win32's
// SuspendThread failing on a dead thread. Bug 2026-11-12 (nk_kill hung once on 2 CPUs): an interrupt
// read iCurrentThread just as a dying thread handed over to the next one, signalled it, and the thread
// exited without handling the signal: the suspender waited for an ack forever. Regression: nk_exitrace.
TInt LxSuspend(SLxThread* aT)
	{
	SLxThreadFull* t = static_cast<SLxThreadFull*>(aT);
	if (t->iState != ELxLive)
		return -1;
	SLxThreadFull* self = static_cast<SLxThreadFull*>(LxSelf());
	TInt prev = __atomic_fetch_add(&t->iSuspend, 1, __ATOMIC_SEQ_CST);
	if (prev == 0)
		{
		if (self) self->iSuspending = t;
		TInt g = __atomic_add_fetch(&t->iGen, 1, __ATOMIC_SEQ_CST);
		Poke(t);		// a thread still parked from the last suspend acknowledges this one
		if (hsys(SYS_tgkill, hgetpid(), t->iTid, KLxSuspendSignal) != 0)
			{
			__atomic_fetch_sub(&t->iSuspend, 1, __ATOMIC_SEQ_CST);
			return -1;
			}
		TInt a;
		while ((a = AtomicLoad(&t->iAckGen)) != g)
			{
			if (t->iState != ELxLive || AtomicLoad(&t->iCtid) == 0)
				{
				// exiting: it will never park; undo our count (nobody resumes a dead thread)
				__atomic_fetch_sub(&t->iSuspend, 1, __ATOMIC_SEQ_CST);
				if (self) self->iSuspending = 0;
				return -1;
				}
			hfutex_wait_us(&t->iAckGen, a, 1000);
			}
		if (self) self->iSuspending = 0;
		}
	return prev;
	}

TInt LxResume(SLxThread* aT)
	{
	SLxThreadFull* t = static_cast<SLxThreadFull*>(aT);
	TInt prev = __atomic_fetch_sub(&t->iSuspend, 1, __ATOMIC_SEQ_CST);
	if (prev == 1)
		Poke(t);
	return prev;
	}

// ---- code probes: an undefined instruction at an address in loaded code; the first hit prints the
// thread, pc and lr, puts the original instruction back and continues (a one-shot breakpoint without a
// debugger). Thumb (bit 0 of the address set): 16-bit UDF 0xde00; ARM: 0xe7f000f0.
struct SProbe { TUint32 iAddr; TUint32 iOrig; TBool iThumb; };
static SProbe Probes[256];
static TInt ProbeCount;
TInt HostedProbeHits;

TInt HostedAddProbe(TUint32 aAddr)
	{
	if (ProbeCount == 256)
		return KErrOverflow;
	TBool thumb = aAddr & 1;
	TUint32 a = aAddr & ~1u;
	TUint32 page = a & ~0xfffu;
	hsys(SYS_mprotect, page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
	SProbe& p = Probes[ProbeCount++];
	p.iAddr = a;
	p.iThumb = thumb;
	if (thumb)
		{
		p.iOrig = *(volatile TUint16*)a;
		*(volatile TUint16*)a = 0xde00 | (ProbeCount & 0xff);
		}
	else
		{
		p.iOrig = *(volatile TUint32*)a;
		*(volatile TUint32*)a = 0xe7f000f0;
		}
	return KErrNone;
	}

static TBool ProbeHit(hucontext* aUc)
	{
	TUint32 pc = aUc->mc.pc;
	for (TInt i = 0; i < ProbeCount; ++i)
		{
		SProbe& p = Probes[i];
		if (p.iAddr == pc)
			{
			SLxThread* me = LxSelf();
			++HostedProbeHits;
			hprintf("PROBE %08x hit: lr %08x sp %08x r0 %08x r1 %08x thread %p\n", pc | p.iThumb, aUc->mc.lr,
			        aUc->mc.sp, aUc->mc.r[0], aUc->mc.r[1], me ? me->iNThread : 0);
			if (p.iThumb)
				*(volatile TUint16*)p.iAddr = (TUint16)p.iOrig;
			else
				*(volatile TUint32*)p.iAddr = p.iOrig;
			hsys(SYS_ARM_cacheflush, p.iAddr, p.iAddr + 4, 0);	// I-cache coherence for the restored code
			return ETrue;
			}
		}
	return EFalse;
	}

// ARM11 (the phone's CPU) accepts encodings ARMv8 AArch32 rejects as UNDEFINED. Emulated here:
// - CLZ with its should-be-one fields (bits 19:16, 11:8) not all ones: ARM11 ignores them. N95 code has
//   0xe160201e (`clz r2, lr`, AknIconServer's runtime-copied rasterizer). Bug 2026-11-12: SIGILL, the
//   kernel faulted (AknIconServer is system-critical). Same as the ARM board's QEMU decoder patch
//   (docs/hacks.md). Regression: nk_exc.
static uint32_t& Reg(hsigcontext& c, TInt n)
	{
	return n < 11 ? c.r[n] : n == 11 ? c.fp : n == 12 ? c.ip : n == 13 ? c.sp : n == 14 ? c.lr : c.pc;
	}

static TBool CondPasses(TUint32 aCond, TUint32 aCpsr)
	{
	TBool n = aCpsr >> 31 & 1, z = aCpsr >> 30 & 1, cf = aCpsr >> 29 & 1, v = aCpsr >> 28 & 1;
	switch (aCond)
		{
	case 0: return z; case 1: return !z; case 2: return cf; case 3: return !cf;
	case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
	case 8: return cf && !z; case 9: return !cf || z; case 10: return n == v; case 11: return n != v;
	case 12: return !z && n == v; case 13: return z || n != v; default: return ETrue;
		}
	}

volatile TUint32 HostedArm11Emulated;	// instructions emulated (tests)

static TBool EmulateArm11(hucontext* aUc)
	{
	hsigcontext& c = aUc->mc;
	if (c.cpsr & 0x20)
		return EFalse;					// Thumb: none known
	TUint32 insn = *(const TUint32*)c.pc;
	if ((insn & 0x0ff000f0u) == 0x01600010u && (insn & 0xf0000000u) != 0xf0000000u)	// CLZ, any SBO bits
		{
		TInt rd = insn >> 12 & 15, rm = insn & 15;
		if (rd == 15 || rm == 15)
			return EFalse;				// unpredictable: leave it to the exception path
		if (CondPasses(insn >> 28, c.cpsr))
			Reg(c, rd) = Reg(c, rm) ? __builtin_clz(Reg(c, rm)) : 32;
		c.pc += 4;
		++HostedArm11Emulated;
		return ETrue;
		}
	return EFalse;
	}

static void FaultHandler(int aSig, hsiginfo* aInfo, hucontext* aUc)
	{
	if (aSig == SIGILL && ProbeHit(aUc))
		return;
	if (aSig == SIGILL && EmulateArm11(aUc))
		return;
	// a nanothread's exception goes to the kernel (ncthrd.cpp); anything else: dump and stop the process
	extern TBool HostedNThreadException(TInt aSig, TUint32 aAddr, hucontext* aUc);
	if (HostedNThreadException(aSig, aInfo->fault.addr, aUc))
		return;
	hsigcontext& c = aUc->mc;
	SLxThread* me = LxSelf();
	hprintf("HOSTED FAULT signal %d addr %08x tid %d nthread %p current %p\n", aSig, aInfo->fault.addr,
	        me ? me->iTid : -1, me ? me->iNThread : 0, TheScheduler.iCurrentThread);
	hprintf("  pc %08x lr %08x sp %08x cpsr %08x%s\n", c.pc, c.lr, c.sp, c.cpsr,
	        me && static_cast<SLxThreadFull*>(me)->iStack && aInfo->fault.addr >= (TUint32)static_cast<SLxThreadFull*>(me)->iStack &&
	        aInfo->fault.addr < (TUint32)static_cast<SLxThreadFull*>(me)->iStack + 0x1000 ? "  STACK OVERFLOW (guard page)" : "");
	for (TInt i = 0; i < 11; i += 4)
		hprintf("  r%d %08x %08x %08x %08x\n", i, c.r[i], c.r[i + 1], i + 2 < 11 ? c.r[i + 2] : c.fp, i + 3 < 11 ? c.r[i + 3] : c.ip);
	// frame chain of the faulting code (built with frame pointers); a looping chain = recursion
	hprintf("  frames:");
	TUint32* fp = (TUint32*)c.fp;
	for (TInt i = 0; i < 40 && fp && ((TUint32)fp & 3) == 0 && (TUint32)fp > 0x1000; ++i)
		{
		hprintf(" %08x", fp[1]);
		TUint32* next = (TUint32*)fp[0];
		if (next <= fp)
			break;
		fp = next;
		}
	hprintf("\n");
	TUint32* sp = (TUint32*)c.sp;
	if (me && c.sp - (TUint32)static_cast<SLxThreadFull*>(me)->iStack < 0x1000 + 32)
		sp = (TUint32*)((TUint32)static_cast<SLxThreadFull*>(me)->iStack + 0x1000);	// overflowed: show the stack's lowest words
	hprintf("  stack %08x %08x %08x %08x %08x %08x %08x %08x\n", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
	hexit(128 + aSig);
	}

static SLxThreadFull TheMainLx;
static SLxThreadFull* volatile AllThreads;
void LxDumpAllThreads();

// SIGQUIT (htest's init on a test timeout): dump every host thread, then exit
static void QuitHandler(int, hsiginfo*, hucontext* aUc)
	{
	SLxThreadFull* me = static_cast<SLxThreadFull*>(LxSelf());
	hprintf("HOSTED SIGQUIT in tid %d (nthread %p) pc %08x lr %08x susp %d gen %d/%d suspending tid %d\n", me ? me->iTid : -1,
	        me ? me->iNThread : 0, aUc->mc.pc, aUc->mc.lr, me ? me->iSuspend : 0, me ? me->iGen : 0,
	        me ? me->iAckGen : 0, me && me->iSuspending ? me->iSuspending->iTid : 0);
	LxDumpAllThreads();
	hexit(124);
	}

static void Register(SLxThreadFull* aT)
	{
	SLxThreadFull* h;
	do	{
		h = AllThreads;
		aT->iNextAll = h;
		} while (!__atomic_compare_exchange_n(&AllThreads, &h, aT, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	}

// ---- watchdog dump: every host thread prints where it is (SIGUSR1 handler) ----
static volatile TInt DumpAck;
static void DumpHandler(int, hsiginfo*, hucontext* aUc)
	{
	SLxThreadFull* me = static_cast<SLxThreadFull*>(LxSelf());
	hsigcontext& c = aUc->mc;
	hprintf("  thread tid %d nthread %p (%s) pc %08x lr %08x sp %08x susp %d gen %d/%d suspending tid %d\n", me ? me->iTid : -1,
	        me ? me->iNThread : 0, me && me->iNThread && me->iNThread->iSpare8 ? (const char*)me->iNThread->iSpare8 : "-",
	        c.pc, c.lr, c.sp, me ? me->iSuspend : 0, me ? me->iGen : 0, me ? me->iAckGen : 0,
	        me && me->iSuspending ? me->iSuspending->iTid : 0);
	__atomic_add_fetch(&DumpAck, 1, __ATOMIC_SEQ_CST);
	}

void LxDumpAllThreads()
	{
	SLxThread* self = LxSelf();
	hprintf("HOSTED DUMP: current %p kernlock %d resched %d dfcpend %d\n", TheScheduler.iCurrentThread,
	        TheScheduler.iKernCSLocked, TheScheduler.iRescheduleNeededFlag, TheScheduler.iDfcPendingFlag);
	TheInterrupt.Dump();
	for (SLxThreadFull* t = AllThreads; t; t = t->iNextAll)
		{
		if (t == self || t->iTid <= 0 || t->iState != ELxLive)
			continue;
		NThread* n = t->iNThread;
		if (n)
			hprintf("  nthread %p (%s) nstate %d wakeup %d inkernel %d pri %d\n", n,
			        n->iSpare8 ? (const char*)n->iSpare8 : "-", n->iNState, n->iWakeup, n->iInKernel, n->iPriority);
		TInt before = DumpAck;
		hsys(SYS_tgkill, hgetpid(), t->iTid, SIGUSR1);
		for (TInt i = 0; i < 200 && DumpAck == before; ++i)
			hsleep_us(1000);
		}
	}

// Bug 2026-11-12: LxInit tested LxSelf() != 0 for "done", but TPIDRURO is not zero at the start of a
// 32-bit process on arm64 Linux (a stale value survives execve): the record was never set up and the
// kernel boot crashed silently. An explicit flag now (declared with LxSelf). Regression: nk_lxinit.

void LxInit()
	{
	if (LxInitDone)
		return;
	LxInitDone = ETrue;
	TheMainLx.iTid = hgettid();
	TheMainLx.iState = ELxLive;
	TheMainLx.iCtid = TheMainLx.iTid;	// not a clone of ours: no CLONE_CHILD_CLEARTID word, never "gone"
	LxSetSelf(&TheMainLx);
	Register(&TheMainLx);
	hsigaction(SIGUSR1, DumpHandler, SA_RESTART);
	hsigaction(3 /* SIGQUIT */, QuitHandler, 0);
	hsigaction(KLxSuspendSignal, SuspendHandler, SA_RESTART);
	static TUint8 mainAlt[0x4000] __attribute__((aligned(16)));
	SetAltStack(mainAlt, sizeof(mainAlt));
	// faults run on the alternate stack: a stack overflow still gets its dump
	hsigaction(SIGSEGV, FaultHandler, SA_ONSTACK);
	hsigaction(SIGBUS, FaultHandler, SA_ONSTACK);
	hsigaction(SIGILL, FaultHandler, SA_ONSTACK);
	}

static void SetAltStack(TUint8* aBase, TUint aSize)
	{
	struct { void* sp; TInt flags; TUint size; } ss = { aBase, 0, aSize };
	hsys(186 /* sigaltstack */, &ss, 0);
	}

static TInt LxThreadStart(TAny* aArg)
	{
	SLxThreadFull* me = static_cast<SLxThreadFull*>(aArg);
	LxSetSelf(me);
	SetAltStack(me->iAltStack, KLxAltSize);
	me->iTid = hgettid();
	// created suspended: wait until resumed (no signal needed: we are not running anything yet)
	Park(me);
	return me->iFunc(me->iArg);
	}

// Thread records live outside the stacks, in pages that are never freed, so the registry (dump,
// reaper) can always walk them; a record is reused once its thread is gone and its stack unmapped.
static const TUint KLxMapSize = KLxStackSize + 0x1000 + KLxAltSize;	// [guard page][stack][alt stack]
static volatile TInt LiveStacks;
static volatile TInt RecordLock;

static SLxThreadFull* NewRecord()
	{
	static SLxThreadFull* page;
	static TInt left;
	if (!left)
		{
		page = (SLxThreadFull*)hmmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (HMAP_FAILED(page))
			return 0;
		left = 0x10000 / sizeof(SLxThreadFull);
		}
	--left;
	SLxThreadFull* r = page++;
	Register(r);
	return r;
	}

// Free the stacks of threads that have fully exited; return a free record (reused or new)
static SLxThreadFull* Reap()
	{
	while (__atomic_exchange_n(&RecordLock, 1, __ATOMIC_ACQUIRE))
		hsys(SYS_sched_yield);
	SLxThreadFull* freeRec = 0;
	for (SLxThreadFull* t = AllThreads; t; t = t->iNextAll)
		{
		if (t->iState == ELxDead && AtomicLoad(&t->iCtid) == 0)
			{
			hmunmap(t->iStack, KLxMapSize);
			t->iStack = 0;
			t->iState = ELxFree;
			__atomic_sub_fetch(&LiveStacks, 1, __ATOMIC_SEQ_CST);
			}
		if (t->iState == ELxFree && t != &TheMainLx && !freeRec)
			freeRec = t;
		}
	if (!freeRec)
		freeRec = NewRecord();
	if (freeRec)
		freeRec->iState = ELxLive;
	__atomic_store_n(&RecordLock, 0, __ATOMIC_RELEASE);
	return freeRec;
	}

TInt HostedLiveStacks()
	{
	return LiveStacks;
	}

static TInt LxThreadBody(TAny* aArg)
	{
	TInt r = LxThreadStart(aArg);
	static_cast<SLxThreadFull*>(aArg)->iState = ELxDead;	// the thread function returned: exiting
	return r;
	}

SLxThread* LxCreateThread(TLxThreadFunction aFunc, TAny* aArg, TBool aRun, NThread* aNThread)
	{
	SLxThreadFull* t = Reap();
	if (!t)
		return 0;
	TUint8* m = (TUint8*)hmmap(0, KLxMapSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(m))
		{
		t->iState = ELxFree;
		return 0;
		}
	hsys(SYS_mprotect, m, 0x1000, 0);
	__atomic_add_fetch(&LiveStacks, 1, __ATOMIC_SEQ_CST);
	SLxThreadFull* next = t->iNextAll;		// keep the registry link
	memclr(t, sizeof(*t));
	t->iNextAll = next;
	t->iState = ELxLive;
	t->iNThread = aNThread;
	t->iFunc = aFunc;
	t->iArg = aArg;
	t->iStack = m;
	t->iSuspend = aRun ? 0 : 1;
	t->iTid = -1;
	t->iAltStack = m + 0x1000 + KLxStackSize;
	if (hthread(LxThreadBody, t, m + 0x1000, KLxStackSize, &t->iCtid) <= 0)
		{
		hmunmap(m, KLxMapSize);
		__atomic_sub_fetch(&LiveStacks, 1, __ATOMIC_SEQ_CST);
		t->iState = ELxFree;
		return 0;
		}
	// the tid is needed by LxSuspend: wait until the thread has published it
	while (AtomicLoad(&t->iTid) == -1)
		hsys(SYS_sched_yield);
	return t;
	}

void LxThreadExit()
	{
	// the stack is unmapped by a later Reap, once Linux has cleared iCtid (the thread is gone)
	static_cast<SLxThreadFull*>(LxSelf())->iState = ELxDead;
	for (;;)
		hsys(SYS_exit, 0);
	}

EXPORT_C SLxThread* CreateLinuxThread(TEmulThreadType aType, TLxThreadFunction aThreadFunc, TAny* aPtr, TBool aRun)
	{
	(void)aType;
	return LxCreateThread(aThreadFunc, aPtr, aRun, 0);
	}
