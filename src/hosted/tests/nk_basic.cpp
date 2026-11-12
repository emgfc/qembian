// nk_basic: the unmodified EKA2 nanokernel (nkern/*.cpp) on the Linux platform layer
// (src/hosted/nkern/linux) in one 32-bit process: P1 of docs/hosted-eka2/roadmap.md.
// Every nanothread is a host thread that runs only when the EKA2 scheduler picked it; the system
// timer is an 'interrupt' thread (StartOfInterrupt/EndOfInterrupt) ticking NTimerQ every 1 ms.
// Checks: initial thread + idle, thread create/resume, fast semaphore ping-pong (exact alternation),
// strict priority (a higher thread always runs first), preemption of a busy thread by a timer-woken
// higher one, round robin between busy equal-priority threads, fast mutex exclusion, DFC queue
// thread, IDFC from interrupt context, one-shot and periodic NTimers, NKern::Sleep accuracy.
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern volatile TUint32 HostedTicks;
extern volatile TUint32 HostedProgress;
extern void HostedStartWatchdog(TInt aSeconds);

static NThread InitialThread;
static TDfcQue* TimerDfcQ;

static NThread* NewThread(const char* aName, NThreadFunction aFunc, TInt aPri, TAny* aArg, TInt aTimeslice = -1)
	{
	NThread* t = new NThread;
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iFunction = aFunc;
	info.iPriority = aPri;
	info.iTimeslice = aTimeslice;
	info.iParameterBlock = (const TUint32*)aArg;
	info.iParameterBlockSize = 0;
	TInt r = NKern::ThreadCreate(t, info);
	HCHECK(r == KErrNone, "create %s: %d", aName, r);
	++HostedProgress;
	t->iSpare8 = (TUint32)aName;
	return t;
	}

static TDfcQue* NewDfcQ(const char* aName, TInt aPri)
	{
	TDfcQue* q = new TDfcQue;
	NThread* t = NewThread(aName, &TDfcQue::ThreadFunction, aPri, q);
	q->iThread = t;
	NKern::ThreadResume(t);
	return q;
	}

// ---- fast semaphore ping-pong ----
static NFastSemaphore PingSem, PongSem;
static volatile TInt Seq[2001];
static volatile TInt SeqN;
static NFastSemaphore DoneSem;

static void Pinger(TAny*)
	{
	PingSem.iOwningThread = NKern::CurrentThread();
	for (TInt i = 0; i < 1000; ++i)
		{
		Seq[SeqN++] = 0;
		NKern::FSSignal(&PongSem);
		NKern::FSWait(&PingSem);
		}
	NKern::FSSignal(&DoneSem);
	}

static void Ponger(TAny*)
	{
	PongSem.iOwningThread = NKern::CurrentThread();
	for (TInt i = 0; i < 1000; ++i)
		{
		NKern::FSWait(&PongSem);
		Seq[SeqN++] = 1;
		NKern::FSSignal(&PingSem);
		}
	NKern::FSSignal(&DoneSem);
	}

// ---- priorities ----
static volatile TInt Order[8];
static volatile TInt OrderN;
static void Recorder(TAny* a)
	{
	Order[OrderN++] = (TInt)a;
	NKern::FSSignal(&DoneSem);
	}

// ---- preemption: a busy low thread, a sleeping high thread ----
static volatile TInt Spin;
static volatile TUint32 SpinCount;
static volatile TInt HighWakes;
static volatile TUint32 HighLate;
static void Busy(TAny*)
	{
	while (Spin)
		++SpinCount;
	NKern::FSSignal(&DoneSem);
	}
static void HighSleeper(TAny*)
	{
	for (TInt i = 0; i < 20; ++i)
		{
		TUint32 t0 = HostedTicks;
		NKern::Sleep(5);
		TUint32 d = HostedTicks - t0;
		if (d > HighLate) HighLate = d;
		++HighWakes;
		}
	Spin = 0;
	NKern::FSSignal(&DoneSem);
	}

// ---- round robin ----
static volatile TUint32 RR[2];
static volatile TInt RRRun;
static void RRWorker(TAny* a)
	{
	TInt me = (TInt)a;
	while (RRRun)
		++RR[me];
	NKern::FSSignal(&DoneSem);
	}

// ---- fast mutex ----
static NFastMutex Mutex;
static volatile TInt Inside, Overlaps, MutexRounds;
static void MutexWorker(TAny*)
	{
	for (TInt i = 0; i < 300; ++i)
		{
		NKern::FMWait(&Mutex);
		if (++Inside != 1) ++Overlaps;
		for (volatile TInt k = 0; k < 50; ++k) {}
		--Inside;
		++MutexRounds;
		NKern::FMSignal(&Mutex);
		if ((i & 15) == 0) NKern::YieldTimeslice();
		}
	NKern::FSSignal(&DoneSem);
	}

// ---- DFCs and timers ----
static volatile TInt DfcRuns, IdfcRuns;
static NThread* DfcThreadSeen;
static void DfcFn(TAny* a)
	{
	++DfcRuns;
	DfcThreadSeen = NKern::CurrentThread();
	NKern::FSSignal((NFastSemaphore*)a);
	}
static void IdfcFn(TAny* a)
	{
	++IdfcRuns;
	TDfc* dfc = (TDfc*)a;
	dfc->Enque();		// an IDFC queues the DFC (the usual interrupt -> IDFC -> DFC chain)
	}

static TInt RaiseIdfc(TAny* a)
	{
	StartOfInterrupt();
	((TDfc*)a)->Add();
	EndOfInterrupt();
	return 0;
	}

static volatile TInt PeriodicCount;
static NTimer* Periodic;
static void PeriodicFn(TAny*)
	{
	if (++PeriodicCount < 10)
		Periodic->Again(2);
	}

static void WaitDone(TInt aCount)
	{
	for (TInt i = 0; i < aCount; ++i)
		{
		NKern::FSWait(&DoneSem);
		++HostedProgress;
		}
	}

static void MainThread(TAny*)
	{
	DoneSem.iOwningThread = NKern::CurrentThread();
	HCHECK(NKern::CurrentThread() != &InitialThread, "main runs as its own nanothread");
	HCHECK(NKern::CurrentContext() == NKern::EThread, "context = thread");

	// 1. ping-pong: strict alternation through the scheduler
	NThread* a = NewThread("Ping", &Pinger, 20, 0);
	NThread* b = NewThread("Pong", &Ponger, 20, 0);
	TUint32 t0 = NKern::FastCounter();
	NKern::ThreadResume(b);
	NKern::ThreadResume(a);
	WaitDone(2);
	TUint32 t1 = NKern::FastCounter();
	TInt alt = 1;
	for (TInt i = 0; i < 2000; ++i)
		if (Seq[i] != (i & 1)) alt = 0;
	HCHECK(SeqN == 2000 && alt, "fast semaphore ping-pong: 2000 steps, strict alternation");
	htest_note("ping-pong: %u us per switch (TCG)", (t1 - t0) / 2000);

	// 2. priorities: resumed while main is above them; when main drops below, highest runs first
	OrderN = 0;
	NThread* r[3];
	r[0] = NewThread("R10", &Recorder, 10, (TAny*)10);
	r[1] = NewThread("R30", &Recorder, 30, (TAny*)30);
	r[2] = NewThread("R20", &Recorder, 20, (TAny*)20);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 40);	// above all three while resuming
	NKern::ThreadResume(r[0]);
	NKern::ThreadResume(r[1]);
	NKern::ThreadResume(r[2]);
	HCHECK(OrderN == 0, "none ran while main is higher (%d)", OrderN);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 5);	// below all three: they run now
	HCHECK(OrderN == 3, "all three ran as soon as main dropped (%d)", OrderN);
	WaitDone(3);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 16);
	HCHECK(OrderN == 3 && Order[0] == 30 && Order[1] == 20 && Order[2] == 10,
	       "priority order 30, 20, 10 (got %d %d %d)", Order[0], Order[1], Order[2]);

	// 2b. immediate preemption: resuming a higher thread from a lower one switches at once
	OrderN = 0;
	NThread* r50 = NewThread("R50", &Recorder, 50, (TAny*)50);
	NKern::ThreadResume(r50);
	HCHECK(OrderN == 1 && Order[0] == 50, "a resumed higher thread ran before ThreadResume returned");
	WaitDone(1);

	// 3. preemption: busy thread at 12, a sleeper at 25 must wake every 5 ticks anyway
	Spin = 1;
	NKern::ThreadSetPriority(NKern::CurrentThread(), 30);
	NThread* busy = NewThread("Busy", &Busy, 12, 0);
	NThread* hi = NewThread("High", &HighSleeper, 25, 0);
	NKern::ThreadResume(busy);
	NKern::ThreadResume(hi);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 11);
	WaitDone(2);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 16);
	HCHECK(HighWakes == 20, "high thread woke 20 times through a busy lower thread");
	HCHECK(SpinCount > 0, "busy thread ran (%u loops)", SpinCount);
	htest_note("worst sleep(5) = %u ticks (TCG, 1 ms tick)", HighLate);

	// 4. round robin: two busy threads at 14 with a 2-tick timeslice share the CPU
	RRRun = 1;
	NThread* w0 = NewThread("RR0", &RRWorker, 14, (TAny*)0, 2);
	NThread* w1 = NewThread("RR1", &RRWorker, 14, (TAny*)1, 2);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 30);
	NKern::ThreadResume(w0);
	NKern::ThreadResume(w1);
	NKern::Sleep(100);		// main (30) sleeps; the two share 100 ticks
	RRRun = 0;
	WaitDone(2);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 16);
	HCHECK(RR[0] > 0 && RR[1] > 0, "round robin: both busy threads progressed (%u, %u)", RR[0], RR[1]);

	// 5. fast mutex: three threads, never two inside
	NThread* m[3];
	for (TInt i = 0; i < 3; ++i)
		m[i] = NewThread("Mtx", &MutexWorker, 18, 0, 1);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 10);
	for (TInt i = 0; i < 3; ++i)
		NKern::ThreadResume(m[i]);
	WaitDone(3);
	NKern::ThreadSetPriority(NKern::CurrentThread(), 16);
	HCHECK(MutexRounds == 900 && Overlaps == 0, "fast mutex: 900 rounds, %d overlaps", Overlaps);

	// 6. DFC on a DFC queue thread, queued by an IDFC raised from the timer interrupt
	NFastSemaphore dfcSem;
	dfcSem.iOwningThread = NKern::CurrentThread();
	TDfcQue* q = NewDfcQ("DfcQ", 27);
	TDfc dfc(&DfcFn, &dfcSem, q, 1);
	TDfc idfc(&IdfcFn, &dfc);
	CreateLinuxThread(EThreadEvent, &RaiseIdfc, &idfc, ETrue);	// an 'interrupt' adds the IDFC
	NKern::FSWait(&dfcSem);
	HCHECK(IdfcRuns == 1, "IDFC added from an interrupt thread ran once");
	HCHECK(DfcRuns == 1 && DfcThreadSeen == q->iThread, "DFC queued by the IDFC ran on its queue's thread");

	// 7. timers: a one-shot after 10 ticks, a periodic restarted 9 times every 2 ticks
	TUint32 k0 = HostedTicks;
	NKern::Sleep(10);
	TUint32 slept = HostedTicks - k0;
	HCHECK(slept >= 10 && slept <= 40, "NKern::Sleep(10) took %u ticks", slept);
	Periodic = new NTimer(&PeriodicFn, 0);
	Periodic->OneShot(2);
	NKern::Sleep(60);
	HCHECK(PeriodicCount == 10, "periodic timer fired 10 times (%d)", PeriodicCount);

	hexit(htest_done());
	}

int main(int, char**)
	{
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iPriority = 0;
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);					// 1 ms tick
	NKern::Init(&InitialThread, info);
	InitialThread.iSpare8 = (TUint32)"Null";
	HCHECK(NKern::CurrentThread() == &InitialThread, "NKern::Init: the initial thread is current");
	TimerDfcQ = NewDfcQ("Timer", 48);
	NTimerQ::Init3(TimerDfcQ);
	HostedStartSystemTimer(1000);
	HostedStartWatchdog(8);
	NThread* m = NewThread("Main", &MainThread, 16, 0);
	NKern::ThreadResume(m);
	for (;;)
		NKern::Idle();
	}
