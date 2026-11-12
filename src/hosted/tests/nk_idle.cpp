// nk_idle: the nanokernel with no ready thread at all.
// Regression for the bug of 2026-11-12: the Win32-derived scheduler asserted in SelectThread when every
// thread was blocked; nkernsa's initial (null) thread calls NKern::Sleep before any other thread
// exists (init_fast_counter). The hosted Reschedule now idles until an interrupt readies a thread.
// Checks: the initial thread sleeps with nothing else to run (1, 5, 20 ticks); all threads blocked and
// an interrupt-raised IDFC -> DFC wakes one; a sleep while another thread sleeps too.
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern volatile TUint32 HostedTicks;

static NThread Initial;
static NFastSemaphore Sem;
static volatile TInt Woken;

static void DfcFn(TAny*)
	{
	NKern::FSSignal(&Sem);
	}

static TInt Interrupter(TAny* a)
	{
	hsleep_us(30000);		// everything is blocked by now
	StartOfInterrupt();
	((TDfc*)a)->Add();
	EndOfInterrupt();
	return 0;
	}

static void Sleeper(TAny*)
	{
	NKern::Sleep(7);
	++Woken;
	}

static TDfcQue* NewDfcQ(TInt aPri)
	{
	TDfcQue* q = new TDfcQue;
	NThread* qt = new NThread;
	SNThreadCreateInfo qi;
	memclr(&qi, sizeof(qi));
	qi.iFunction = &TDfcQue::ThreadFunction;
	qi.iPriority = aPri;
	qi.iTimeslice = -1;
	qi.iParameterBlock = (const TUint32*)q;
	HCHECK(NKern::ThreadCreate(qt, qi) == KErrNone, "DFC thread created");
	q->iThread = qt;
	NKern::ThreadResume(qt);
	return q;
	}

int main(int, char**)
	{
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	NTimerQ::Init3(NewDfcQ(48));		// the timer DFC queue thread: blocked waiting for DFCs
	HostedStartSystemTimer(1000);
	// the initial thread sleeps while every other thread is blocked too: nothing is ready
	for (TInt n = 1; n <= 20; n += (n < 5 ? 4 : 15))
		{
		TUint32 t0 = HostedTicks;
		NKern::Sleep(n);
		TUint32 d = HostedTicks - t0;
		HCHECK(d >= (TUint32)n && d <= (TUint32)n + 30, "nothing ready: Sleep(%d) took %u ticks", n, d);
		}

	// a second thread sleeping at the same time
	NThread* t = new NThread;
	SNThreadCreateInfo ti;
	memclr(&ti, sizeof(ti));
	ti.iFunction = &Sleeper;
	ti.iPriority = 20;
	ti.iTimeslice = -1;
	HCHECK(NKern::ThreadCreate(t, ti) == KErrNone, "thread created");
	NKern::ThreadResume(t);		// runs at once (higher) and sleeps
	NKern::Sleep(15);
	HCHECK(Woken == 1, "both slept, the other woke first");

	// every thread blocked on a semaphore; only an interrupt -> IDFC -> DFC can wake us
	TDfcQue* q = NewDfcQ(27);
	Sem.iOwningThread = NKern::CurrentThread();
	TDfc dfc(&DfcFn, 0, q, 1);
	struct SChain { static void Fn(TAny* a) { ((TDfc*)a)->Enque(); } };
	TDfc chain(&SChain::Fn, &dfc);
	CreateLinuxThread(EThreadEvent, &Interrupter, &chain, ETrue);
	NKern::FSWait(&Sem);
	HCHECK(ETrue, "woken through interrupt -> IDFC -> DFC while every thread was blocked");
	hexit(htest_done());
	}
