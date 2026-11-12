// Hosted EKA2 nanokernel: what the nanokernel needs from the layers above it when it runs alone
// (no EKERN, no variant): debug output (KPrintf / Kern::Printf style, %T %M %C), the crash handler,
// a heap for operator new, and the 'system timer' interrupt thread (1 ms tick into NTimerQ).
// Original work for qembian.

#include "nk_priv.h"

// ---- debug output: the Symbian kernel printf conventions ----
// %d %i %u %x %X %c %s %p with flags 0/-/width; 'l' or 'll' = 64-bit; %T a nanothread (its name from
// iSpare8 when set), %M a fast mutex, %C %G other kernel objects (printed as addresses)
static void Emit(const char* aFmt, va_list aList)
	{
	char buf[512];
	TInt n = 0;
	const TInt max = sizeof(buf) - 2;
	for (const char* p = aFmt; *p && n < max; ++p)
		{
		if (*p != '%') { buf[n++] = *p; continue; }
		++p;
		TBool zero = EFalse, left = EFalse;
		TInt width = 0, lng = 0;
		for (;; ++p)
			{
			if (*p == '0') zero = ETrue;
			else if (*p == '-') left = ETrue;
			else break;
			}
		while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
		while (*p == 'l' || *p == 'h') { if (*p == 'l') ++lng; ++p; }
		char tmp[48]; TInt t = 0;
		switch (*p)
			{
		case 0: --p; continue;
		case '%': tmp[t++] = '%'; break;
		case 'c': tmp[t++] = (char)va_arg(aList, TInt); break;
		case 's': { const char* s = va_arg(aList, const char*); if (!s) s = "(null)";
			TInt l = strlen(s); while (!left && width-- > l && n < max) buf[n++] = ' ';
			while (*s && n < max) buf[n++] = *s++; while (left && width-- > l && n < max) buf[n++] = ' '; continue; }
		case 'T': { NThread* th = va_arg(aList, NThread*);
			const char* nm = th ? (const char*)th->iSpare8 : 0;
			t = nm ? hsnprintf(tmp, sizeof(tmp), "%s", nm) : hsnprintf(tmp, sizeof(tmp), "T%p", th); break; }
		case 'M': case 'C': case 'G': case 'O': case 'p':
			t = hsnprintf(tmp, sizeof(tmp), "%08x", (TUint32)va_arg(aList, TAny*)); break;
		case 'd': case 'i':
			t = lng ? hsnprintf(tmp, sizeof(tmp), "%lld", va_arg(aList, TInt64))
			        : hsnprintf(tmp, sizeof(tmp), "%d", va_arg(aList, TInt)); break;
		case 'u':
			t = lng ? hsnprintf(tmp, sizeof(tmp), "%llu", va_arg(aList, TUint64))
			        : hsnprintf(tmp, sizeof(tmp), "%u", va_arg(aList, TUint)); break;
		case 'x': case 'X':
			t = lng ? hsnprintf(tmp, sizeof(tmp), "%llx", va_arg(aList, TUint64))
			        : hsnprintf(tmp, sizeof(tmp), "%x", va_arg(aList, TUint)); break;
		default: tmp[t++] = '?'; break;
			}
		if (t > (TInt)sizeof(tmp) - 1) t = sizeof(tmp) - 1;
		while (!left && width-- > t && n < max) buf[n++] = zero ? '0' : ' ';
		for (TInt i = 0; i < t && n < max; ++i) buf[n++] = tmp[i];
		while (left && width-- > t && n < max) buf[n++] = ' ';
		}
	buf[n++] = '\n';
	hwrite(1, buf, n);
	}

__attribute__((weak)) void KPrintf(const char* aFmt, ...)
	{
	va_list l; va_start(l, aFmt);
	Emit(aFmt, l);
	va_end(l);
	}

// ---- crash ----
__attribute__((weak)) void NKCrashHandler(TInt aPhase, const TAny* a0, TInt a1)
	{
	if (aPhase == 0)
		return;
	const char* cat = (const char*)a0;
	hprintf("NKERN FAULT: %s %d (0x%x) thread %p\n", cat ? cat : "?", a1, a1, TheScheduler.iCurrentThread);
	}

// ---- heap (weak: the kernel layer has its own, on the kernel heap: common/gcchelp.cpp): one region, first-fit free list (enough for tests; the hosted kernel gets a real one) ----
struct SHdr { TUint32 iSize; SHdr* iNext; };
static SHdr* FreeList;
static TUint8* HeapTop;
static TUint8* HeapEnd;
static volatile TInt HeapLock;

static void Lock() { while (__atomic_exchange_n(&HeapLock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Unlock() { __atomic_store_n(&HeapLock, 0, __ATOMIC_RELEASE); }

extern "C" TAny* malloc(TUint aSize)
	{
	TUint need = (aSize + sizeof(SHdr) + 15) & ~15u;
	Lock();
	if (!HeapTop)
		{
		HeapTop = (TUint8*)hmmap(0, 0x4000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		HeapEnd = HeapTop + 0x4000000;
		}
	SHdr** pp = &FreeList;
	for (SHdr* h = FreeList; h; pp = &h->iNext, h = h->iNext)
		if (h->iSize >= need)
			{
			*pp = h->iNext;
			Unlock();
			return h + 1;
			}
	TAny* r = 0;
	if (HeapTop + need <= HeapEnd)
		{
		SHdr* h = (SHdr*)HeapTop;
		h->iSize = need;
		HeapTop += need;
		r = h + 1;
		}
	Unlock();
	if (r)
		memclr(r, need - sizeof(SHdr));
	return r;
	}

extern "C" void free(TAny* aPtr)
	{
	if (!aPtr)
		return;
	SHdr* h = (SHdr*)aPtr - 1;
	Lock();
	h->iNext = FreeList;
	FreeList = h;
	Unlock();
	}

__attribute__((weak)) TAny* operator new(TUint aSize) __NO_THROW { TAny* p = malloc(aSize); if (p) memclr(p, aSize); return p; }
__attribute__((weak)) TAny* operator new[](TUint aSize) __NO_THROW { TAny* p = malloc(aSize); if (p) memclr(p, aSize); return p; }
__attribute__((weak)) void operator delete(TAny* aPtr) __NO_THROW { free(aPtr); }
__attribute__((weak)) void operator delete[](TAny* aPtr) __NO_THROW { free(aPtr); }
__attribute__((weak)) void operator delete(TAny* aPtr, TUint) __NO_THROW { free(aPtr); }
__attribute__((weak)) void operator delete[](TAny* aPtr, TUint) __NO_THROW { free(aPtr); }

// C++ runtime bits the compiler may reference
extern "C" void __cxa_pure_virtual() { FAULT(); }
extern "C" int __aeabi_atexit(void*, void (*)(void*), void*) { return 0; }
extern "C" void* __dso_handle = 0;

extern NTimerQ TheTimerQ;

// ---- the system timer: an 'interrupt' thread ticking NTimerQ every aPeriodUs ----
static TInt TimerPeriodUs;
static volatile TInt TimerStop;
volatile TUint32 HostedTicks;			// ticks elapsed (suppressed ones included)

volatile TUint32 HostedTicksSkipped;
TInt HostedTickSkip = 1;
static const TInt KMaxSkip = 7;

static TInt TimerThread(TAny*)
	{
	TInt skipped = 0;
	TUint64 next = hnow_ns();
	while (!TimerStop)
		{
		next += (TUint64)TimerPeriodUs * 1000;
		TUint64 now = hnow_ns();
		if (next > now)
			hsleep_us((TUint32)((next - now) / 1000));
		else if (now - next > 100000000ull)
			next = now;		// more than 100 ms behind (a stopped VM): do not replay every tick
		// Tick suppression (as a port's idle tick suppression: NTimerQ::IdleTime/Advance): under the interrupt
		// lock only, without stopping the running nanothread, skip a tick that expires nothing; take the real
		// interrupt when a timer (or a queue transfer) is due and at least every KMaxSkip ticks, catching up the
		// timeslice ticks then. A full tick suspends and resumes the running thread (signals, futexes): at
		// 1 kHz that was the largest kernel cost under TCG. HostedTickSkip=0 (htest --env TICK_SKIP=0): off.
		if (HostedTickSkip && skipped < KMaxSkip)
			{
			TInt irq = NKern::DisableAllInterrupts();
			TBool due = TheTimerQ.IdleTime() <= 1;
			if (!due)
				TheTimerQ.Advance(1);
			NKern::RestoreInterrupts(irq);
			if (!due)
				{
				++skipped;
				++HostedTicks;
				++HostedTicksSkipped;
				continue;
				}
			}
		StartOfInterrupt();
		++HostedTicks;
		for (; skipped > 0; --skipped)
			TheScheduler.TimesliceTick();
		TheTimerQ.Tick();
		EndOfInterrupt();
		}
	return 0;
	}

void HostedStartSystemTimer(TInt aPeriodUs)
	{
	TimerPeriodUs = aPeriodUs;
	CreateLinuxThread(EThreadEvent, &TimerThread, 0, ETrue);
	}

void HostedStopSystemTimer()
	{
	TimerStop = 1;
	}

// ---- the hooks the nanokernel calls into EKERN / the variant for (weak: a test suite may bring its own) ----
TInt HostedDebugMask = 0;			// __KTRACE_OPT bits (nk_trace.h KSCHED etc.)

__attribute__((weak)) EXPORT_C TInt KDebugMask()
	{
	return HostedDebugMask;
	}

__attribute__((weak)) EXPORT_C TBool KDebugNum(TInt aBitNum)
	{
	return aBitNum >= 0 && aBitNum < 32 && (HostedDebugMask & (1 << aBitNum));
	}

__attribute__((weak)) TInt KCrazySchedulerEnabled()
	{
	return 0;
	}

extern "C" {
// the call chain from the frame pointers (everything is built with -fno-omit-frame-pointer; ARM-state
// clang frames: [fp] = caller's fp, [fp + 4] = return address); symbolise with llvm-addr2line
void HostedBacktrace()
	{
	TUint32* fp = (TUint32*)__builtin_frame_address(0);
	hprintf("  backtrace:");
	for (TInt i = 0; i < 24 && fp && ((TUint32)fp & 3) == 0; ++i)
		{
		TUint32 ret = fp[1];
		TUint32* next = (TUint32*)fp[0];
		hprintf(" %08x", ret);
		if (next <= fp || (TUint32)next - (TUint32)fp > 0x100000)
			break;
		fp = next;
		}
	hprintf("\n");
	}

// Bug 2026-11-12: NKFault printed NKern::CurrentContext(), which asserts (-> NKFault) when the faulting
// state is inconsistent: endless recursion, a stack overflow instead of the first fault's report.
// Now it prints only raw state, and a nested fault just exits. Regression: nk_fault.
static volatile TInt InFault;
__attribute__((weak)) EXPORT_C void NKFault(const char* aFile, TInt aLine)
	{
	if (__atomic_exchange_n(&InFault, 1, __ATOMIC_SEQ_CST))
		{
		hprintf("NKERN FAULT (nested) %s:%d\n", aFile ? aFile : "?", aLine);
		hexit(101);
		}
	SLxThread* me = LxSelf();
	hprintf("NKERN FAULT %s:%d current %p host tid %d nthread %p kernlock %d\n", aFile ? aFile : "?", aLine,
	        TheScheduler.iCurrentThread, me ? me->iTid : -1, me ? me->iNThread : 0, TheScheduler.iKernCSLocked);
	HostedBacktrace();
	hexit(101);
	}

__attribute__((weak)) void NKIdle(TUint32)
	{
	}

__attribute__((weak)) void ExcFault(TAny* aContext)
	{
	hprintf("NKERN exception fault, context %p\n", aContext);
	hexit(102);
	}
}

__attribute__((weak)) TBool BTrace::IsSupported(TUint aCategory)
	{
	return aCategory <= 255;
	}

__attribute__((weak)) TBool SBTraceData::CheckFilter2(TUint32)
	{
	return EFalse;
	}

// ---- watchdog: if the process makes no progress mark for aSeconds, dump every thread and exit ----
extern void LxDumpAllThreads();
volatile TUint32 HostedProgress;
static TInt WatchdogSeconds;

static TInt Watchdog(TAny*)
	{
	TUint32 last = HostedProgress - 1;
	TInt still = 0;
	for (;;)
		{
		hsleep_us(1000000);
		TUint32 p = HostedProgress;
		still = (p == last) ? still + 1 : 0;
		last = p;
		if (still >= WatchdogSeconds)
			{
			hprintf("HOSTED WATCHDOG: no progress for %d s (mark %u, ticks %u)\n", still, p, HostedTicks);
			LxDumpAllThreads();
			hexit(124);
			}
		}
	}

void HostedStartWatchdog(TInt aSeconds)
	{
	WatchdogSeconds = aSeconds;
	CreateLinuxThread(EThreadEvent, &Watchdog, 0, ETrue);
	}
